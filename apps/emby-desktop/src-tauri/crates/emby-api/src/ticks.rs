//! Emby tick conversions.
//!
//! Emby measures time in 100-nanosecond ticks: one second is 10,000,000 ticks.

/// Number of Emby ticks in one second.
pub const TICKS_PER_SECOND: i64 = 10_000_000;

/// Converts seconds to Emby ticks. Negative, NaN and infinite values become 0
/// so a bad position from the player cannot overflow `PositionTicks`.
pub fn seconds_to_ticks(seconds: f64) -> i64 {
    if !seconds.is_finite() || seconds <= 0.0 {
        return 0;
    }
    // `as i64` saturates on overflow (Rust >= 1.45).
    (seconds * TICKS_PER_SECOND as f64).round() as i64
}

/// Converts Emby ticks to fractional seconds.
pub fn ticks_to_seconds(ticks: i64) -> f64 {
    ticks as f64 / TICKS_PER_SECOND as f64
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn converts_seconds_to_ticks() {
        assert_eq!(seconds_to_ticks(0.0), 0);
        assert_eq!(seconds_to_ticks(1.0), 10_000_000);
        assert_eq!(seconds_to_ticks(12.5), 125_000_000);
        assert_eq!(seconds_to_ticks(0.000_000_1), 1);
        assert_eq!(ticks_to_seconds(seconds_to_ticks(42.75)), 42.75);
    }

    #[test]
    fn clamps_invalid_seconds() {
        assert_eq!(seconds_to_ticks(-3.0), 0);
        assert_eq!(seconds_to_ticks(f64::NAN), 0);
        assert_eq!(seconds_to_ticks(f64::INFINITY), 0);
        assert_eq!(seconds_to_ticks(f64::NEG_INFINITY), 0);
        // absurdly large but finite values saturate instead of wrapping
        assert_eq!(seconds_to_ticks(1e30), i64::MAX);
    }

    #[test]
    fn converts_ticks_to_seconds() {
        assert_eq!(ticks_to_seconds(0), 0.0);
        assert_eq!(ticks_to_seconds(10_000_000), 1.0);
        assert_eq!(ticks_to_seconds(5_962_000_000), 596.2);
        assert_eq!(ticks_to_seconds(-10_000_000), -1.0);
    }
}
