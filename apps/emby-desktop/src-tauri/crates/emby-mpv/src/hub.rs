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
    pub fn publish(&self, snapshot: PlayerSnapshot) -> PlayerSnapshot {
        let (snapshot, sinks) = {
            let mut inner = self.lock();
            Self::commit(&mut inner, snapshot)
        };
        self.fan_out(&snapshot, &sinks);
        snapshot
    }

    /// Mutate the current snapshot and publish it.
    ///
    /// Read, mutate and store happen under one lock, so a `publish` from the
    /// mpv thread can never land in between and be overwritten by a stale copy
    /// (e.g. a volume change resurrecting `playing` right after `ended`).
    /// `mutate` must not call back into the hub: the lock is not reentrant.
    pub fn update(&self, mutate: impl FnOnce(&mut PlayerSnapshot)) -> PlayerSnapshot {
        let (snapshot, sinks) = {
            let mut inner = self.lock();
            let mut snapshot = inner.latest.clone();
            mutate(&mut snapshot);
            Self::commit(&mut inner, snapshot)
        };
        self.fan_out(&snapshot, &sinks);
        snapshot
    }

    /// Assign `seq`, store as latest and return the sinks to notify. Caller
    /// holds the lock.
    fn commit(
        inner: &mut HubInner,
        mut snapshot: PlayerSnapshot,
    ) -> (PlayerSnapshot, Vec<Arc<dyn SnapshotSink>>) {
        inner.seq = inner.seq.wrapping_add(1);
        snapshot.seq = inner.seq;
        inner.latest = snapshot.clone();
        let sinks: Vec<Arc<dyn SnapshotSink>> =
            inner.sinks.iter().map(|(_, sink)| sink.clone()).collect();
        (snapshot, sinks)
    }

    /// Deliver outside the lock, then prune sinks that reported a failed send
    /// (dead channels).
    fn fan_out(&self, snapshot: &PlayerSnapshot, sinks: &[Arc<dyn SnapshotSink>]) {
        for sink in sinks {
            sink.publish(snapshot.clone());
        }
        if sinks.iter().any(|sink| !sink.is_alive()) {
            self.lock().sinks.retain(|(_, sink)| sink.is_alive());
        }
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
    use crate::types::{PlayerSnapshot, STATUS_ENDED, STATUS_PAUSED, STATUS_PLAYING};
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
    fn update_is_atomic_against_a_concurrent_publish() {
        let hub = Arc::new(SnapshotHub::new());
        hub.update(|s| s.status = STATUS_PLAYING.to_string());

        let mut worker = None;
        hub.update(|s| {
            // The mpv thread publishes `ended` while this update is mid-way.
            let other = Arc::clone(&hub);
            worker = Some(std::thread::spawn(move || {
                let mut ended = PlayerSnapshot::default();
                ended.status = STATUS_ENDED.to_string();
                other.publish(ended);
            }));
            // Give it time to run if it could; with the lock held it cannot.
            std::thread::sleep(std::time::Duration::from_millis(50));
            s.volume = 30.0;
        });
        worker.unwrap().join().unwrap();

        // The concurrent publish happened after the update, not inside it, so
        // it is the latest state. A stale copy must not have overwritten it.
        let latest = hub.latest();
        assert_eq!(latest.seq, 3);
        assert_eq!(latest.status, STATUS_ENDED);
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
