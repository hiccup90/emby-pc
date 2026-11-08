//! Snapshot fan-out: keeps the newest snapshot, assigns `seq`, and pushes to
//! every registered sink (Tauri `Channel`s in the app layer).
//!
//! This is the only place `seq` is assigned, so `seq` is monotonic across
//! opens/stops/subscriptions and a stale snapshot can never overwrite a newer
//! one in the UI.

use std::sync::{Arc, Mutex, MutexGuard};

use crate::types::PlayerSnapshot;

/// Destination for snapshots. The Tauri layer implements this over
/// `tauri::ipc::Channel<PlayerSnapshot>`.
pub trait SnapshotSink: Send + Sync + 'static {
    fn publish(&self, snapshot: PlayerSnapshot);

    /// Returning `false` makes the hub drop this sink on the next publish.
    /// Used when a webview channel died (page reload / window closed), so a
    /// long session does not accumulate dead sinks.
    fn is_alive(&self) -> bool {
        true
    }
}

struct HubInner {
    latest: PlayerSnapshot,
    seq: u64,
    sinks: Vec<(u64, Arc<dyn SnapshotSink>)>,
    next_sink_id: u64,
}

pub struct SnapshotHub {
    inner: Mutex<HubInner>,
}

impl Default for SnapshotHub {
    fn default() -> Self {
        Self::new()
    }
}

impl SnapshotHub {
    pub fn new() -> Self {
        Self {
            inner: Mutex::new(HubInner {
                latest: PlayerSnapshot::default(),
                seq: 0,
                sinks: Vec::new(),
                next_sink_id: 1,
            }),
        }
    }

    fn lock(&self) -> MutexGuard<'_, HubInner> {
        // A panic while holding the hub lock must not poison playback forever.
        self.inner.lock().unwrap_or_else(|poison| poison.into_inner())
    }

    pub fn latest(&self) -> PlayerSnapshot {
        self.lock().latest.clone()
    }

    /// Assign a fresh `seq`, store, and fan out. Sinks are invoked after the
    /// lock is released so a slow channel cannot block `player_snapshot`.
    pub fn publish(&self, mut snapshot: PlayerSnapshot) -> PlayerSnapshot {
        let sinks: Vec<Arc<dyn SnapshotSink>> = {
            let mut inner = self.lock();
            inner.seq = inner.seq.wrapping_add(1);
            snapshot.seq = inner.seq;
            inner.latest = snapshot.clone();
            inner.sinks.iter().map(|(_, sink)| sink.clone()).collect()
        };
        for sink in &sinks {
            sink.publish(snapshot.clone());
        }
        // Prune sinks that reported a failed send (dead channels).
        if sinks.iter().any(|sink| !sink.is_alive()) {
            self.lock().sinks.retain(|(_, sink)| sink.is_alive());
        }
        snapshot
    }

    /// Mutate the current snapshot and publish it.
    pub fn update(&self, mutate: impl FnOnce(&mut PlayerSnapshot)) -> PlayerSnapshot {
        let mut snapshot = self.latest();
        mutate(&mut snapshot);
        self.publish(snapshot)
    }

    /// Register a sink and immediately push the current snapshot to it
    /// (`player_subscribe` contract: first message arrives without waiting).
    pub fn add_sink(&self, sink: Arc<dyn SnapshotSink>) -> u64 {
        let (id, latest) = {
            let mut inner = self.lock();
            let id = inner.next_sink_id;
            inner.next_sink_id = inner.next_sink_id.wrapping_add(1).max(1);
            let latest = inner.latest.clone();
            inner.sinks.push((id, sink.clone()));
            (id, latest)
        };
        sink.publish(latest);
        id
    }

    /// Replace the registered sink with `sink` and push the current snapshot.
    ///
    /// `player_subscribe` has no unsubscribe counterpart: the UI subscribes
    /// again on every visit to the player page. Keeping the old channels would
    /// fan every snapshot out N times and keep dead webviews alive, so the new
    /// subscription is the only one that survives.
    pub fn set_sink(&self, sink: Arc<dyn SnapshotSink>) -> u64 {
        let (id, latest) = {
            let mut inner = self.lock();
            let id = inner.next_sink_id;
            inner.next_sink_id = inner.next_sink_id.wrapping_add(1).max(1);
            inner.sinks.clear();
            let latest = inner.latest.clone();
            inner.sinks.push((id, sink.clone()));
            (id, latest)
        };
        sink.publish(latest);
        id
    }

    pub fn remove_sink(&self, id: u64) {
        self.lock().sinks.retain(|(sink_id, _)| *sink_id != id);
    }

    pub fn sink_count(&self) -> usize {
        self.lock().sinks.len()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::types::{PlayerSnapshot, STATUS_PAUSED, STATUS_PLAYING};
    use std::sync::atomic::{AtomicUsize, Ordering};

    struct CountingSink {
        count: Arc<AtomicUsize>,
        last_status: Arc<Mutex<String>>,
    }

    struct MortalSink {
        alive: std::sync::atomic::AtomicBool,
        delivered: AtomicUsize,
    }

    impl MortalSink {
        fn new() -> Self {
            Self {
                alive: std::sync::atomic::AtomicBool::new(true),
                delivered: AtomicUsize::new(0),
            }
        }
    }

    impl SnapshotSink for MortalSink {
        fn publish(&self, _snapshot: PlayerSnapshot) {
            self.delivered.fetch_add(1, Ordering::SeqCst);
            // Simulates a `Channel::send` failure for a gone webview.
            self.alive.store(false, Ordering::SeqCst);
        }

        fn is_alive(&self) -> bool {
            self.alive.load(Ordering::SeqCst)
        }
    }

    impl CountingSink {
        fn new() -> (Self, Arc<AtomicUsize>, Arc<Mutex<String>>) {
            let count = Arc::new(AtomicUsize::new(0));
            let status = Arc::new(Mutex::new(String::new()));
            (
                Self {
                    count: count.clone(),
                    last_status: status.clone(),
                },
                count,
                status,
            )
        }
    }

    impl SnapshotSink for CountingSink {
        fn publish(&self, snapshot: PlayerSnapshot) {
            self.count.fetch_add(1, Ordering::SeqCst);
            if let Ok(mut status) = self.last_status.lock() {
                *status = snapshot.status;
            }
        }
    }

    #[test]
    fn seq_is_monotonic_and_assigned_by_hub() {
        let hub = SnapshotHub::new();
        let mut snapshot = PlayerSnapshot::default();
        snapshot.seq = 999; // caller-supplied seq must be overwritten
        let first = hub.publish(snapshot.clone());
        let second = hub.publish(snapshot);
        assert_eq!(first.seq, 1);
        assert_eq!(second.seq, 2);
        assert_eq!(hub.latest().seq, 2);
    }

    #[test]
    fn update_mutates_and_bumps_seq() {
        let hub = SnapshotHub::new();
        let snap = hub.update(|s| s.status = STATUS_PLAYING.to_string());
        assert_eq!(snap.status, STATUS_PLAYING);
        assert_eq!(snap.seq, 1);
        assert_eq!(hub.latest().status, STATUS_PLAYING);
    }

    #[test]
    fn new_sink_receives_current_snapshot_immediately() {
        let hub = SnapshotHub::new();
        hub.update(|s| s.status = STATUS_PAUSED.to_string());
        let (sink, count, status) = CountingSink::new();
        let id = hub.add_sink(Arc::new(sink));
        assert_eq!(count.load(Ordering::SeqCst), 1);
        assert_eq!(status.lock().unwrap().as_str(), STATUS_PAUSED);
        assert_eq!(hub.sink_count(), 1);

        hub.update(|s| s.status = STATUS_PLAYING.to_string());
        assert_eq!(count.load(Ordering::SeqCst), 2);

        hub.remove_sink(id);
        hub.update(|s| s.status = STATUS_PAUSED.to_string());
        assert_eq!(count.load(Ordering::SeqCst), 2);
        assert_eq!(hub.sink_count(), 0);
    }

    #[test]
    fn dead_sinks_are_pruned_after_a_failed_publish() {
        let hub = SnapshotHub::new();
        let sink = Arc::new(MortalSink::new());
        hub.add_sink(sink.clone());
        assert_eq!(hub.sink_count(), 1);

        // First publish reaches the sink, which then reports itself dead.
        hub.publish(PlayerSnapshot::default());
        assert_eq!(hub.sink_count(), 0);
        let delivered = sink.delivered.load(Ordering::SeqCst);

        // No further deliveries once pruned.
        hub.publish(PlayerSnapshot::default());
        assert_eq!(sink.delivered.load(Ordering::SeqCst), delivered);
    }

    #[test]
    fn set_sink_replaces_the_previous_subscription() {
        let hub = SnapshotHub::new();
        let (old, old_count, _) = CountingSink::new();
        let (new, new_count, _) = CountingSink::new();
        hub.set_sink(Arc::new(old));
        assert_eq!(hub.sink_count(), 1);
        assert_eq!(old_count.load(Ordering::SeqCst), 1); // current snapshot

        hub.set_sink(Arc::new(new));
        assert_eq!(hub.sink_count(), 1);
        assert_eq!(new_count.load(Ordering::SeqCst), 1);

        hub.publish(PlayerSnapshot::default());
        assert_eq!(old_count.load(Ordering::SeqCst), 1, "old channel must be gone");
        assert_eq!(new_count.load(Ordering::SeqCst), 2);
    }

    #[test]
    fn a_fresh_hub_reports_a_valid_idle_snapshot() {
        let snapshot = SnapshotHub::new().latest();
        assert_eq!(snapshot.seq, 0);
        assert_eq!(snapshot.status, crate::types::STATUS_IDLE);
        assert_eq!(snapshot.position_seconds, 0.0);
        assert_eq!(snapshot.duration_seconds, 0.0);
        assert!(snapshot.audio_tracks.is_empty() && snapshot.subtitle_tracks.is_empty());
        assert!(snapshot.error.is_none());
        assert_eq!(snapshot.volume, 100.0);
        assert!(!snapshot.muted);
    }

    #[test]
    fn all_sinks_see_every_snapshot() {
        let hub = SnapshotHub::new();
        let (a, count_a, _) = CountingSink::new();
        let (b, count_b, _) = CountingSink::new();
        hub.add_sink(Arc::new(a));
        hub.add_sink(Arc::new(b));
        hub.publish(PlayerSnapshot::default());
        assert_eq!(count_a.load(Ordering::SeqCst), 2); // initial + publish
        assert_eq!(count_b.load(Ordering::SeqCst), 2);
    }
}
