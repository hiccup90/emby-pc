//! Monotonic sequence latch for `VideoSync` hand-offs.
//!
//! `video_sync` can be delivered out of order (IPC + `PostMessage`), so the
//! newest `seq` wins. Equal sequences are accepted: the UI may legitimately
//! re-send the same frame after a resize.

#[derive(Debug, Default)]
pub struct SeqLatch {
    last: Option<u64>,
}

impl SeqLatch {
    pub fn new() -> Self {
        Self { last: None }
    }

    /// Returns `true` when `seq` is not older than the last accepted value.
    pub fn accept(&mut self, seq: u64) -> bool {
        match self.last {
            Some(last) if seq < last => false,
            _ => {
                self.last = Some(seq);
                true
            }
        }
    }

    pub fn last(&self) -> Option<u64> {
        self.last
    }

    /// Start a new generation. Used when the UI sends `seq == 0`, which means
    /// "my counter restarted" (page remount), not an old snapshot.
    pub fn reset(&mut self) {
        self.last = None;
    }

    /// Next synthetic sequence, used internally for "hide" requests.
    pub fn next(&mut self) -> u64 {
        let seq = self.last.map(|last| last.saturating_add(1)).unwrap_or(1);
        self.last = Some(seq);
        seq
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn newer_sequence_wins() {
        let mut latch = SeqLatch::new();
        assert!(latch.accept(1));
        assert!(latch.accept(5));
        assert_eq!(latch.last(), Some(5));
    }

    #[test]
    fn stale_sequence_is_dropped() {
        let mut latch = SeqLatch::new();
        assert!(latch.accept(10));
        assert!(!latch.accept(9));
        assert!(!latch.accept(0));
        assert_eq!(latch.last(), Some(10));
    }

    #[test]
    fn duplicate_sequence_is_accepted_idempotently() {
        let mut latch = SeqLatch::new();
        assert!(latch.accept(3));
        assert!(latch.accept(3));
        assert_eq!(latch.last(), Some(3));
    }

    #[test]
    fn synthetic_hide_sequence_is_strictly_newer() {
        let mut latch = SeqLatch::new();
        assert!(latch.accept(41));
        let hide = latch.next();
        assert_eq!(hide, 42);
        assert_eq!(latch.next(), 43);
        assert!(latch.accept(44));
        assert!(!latch.accept(43));
    }

    #[test]
    fn empty_latch_accepts_zero() {
        let mut latch = SeqLatch::new();
        assert!(latch.accept(0));
        assert_eq!(latch.last(), Some(0));
    }

    #[test]
    fn reset_allows_a_remounted_ui_to_restart_its_counter() {
        let mut latch = SeqLatch::new();
        assert!(latch.accept(500));
        assert!(!latch.accept(0), "without reset a restarted counter is stale");
        latch.reset();
        assert!(latch.accept(0));
        assert!(latch.accept(1));
        assert!(!latch.accept(0));
    }
}
