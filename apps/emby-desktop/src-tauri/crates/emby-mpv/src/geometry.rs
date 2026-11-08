//! CSS-pixel → physical-pixel placement math for the embedded video window.
//!
//! The webview sends a rectangle in CSS pixels plus `devicePixelRatio`. The
//! Win32 child window needs physical pixels. Both edges are rounded
//! independently (`left = round(x * dpr)`, `right = round((x + w) * dpr)`) so
//! a fractional offset does not accumulate into a shrinking/overlapping
//! rectangle, then the result is clamped to the parent client area.

use crate::types::VideoSync;

/// Physical-pixel rectangle ready for `SetWindowPos` (width/height > 0).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct PixelRect {
    pub left: i32,
    pub top: i32,
    pub width: i32,
    pub height: i32,
}

impl PixelRect {
    pub fn right(&self) -> i32 {
        self.left.saturating_add(self.width)
    }

    pub fn bottom(&self) -> i32 {
        self.top.saturating_add(self.height)
    }
}

/// Convert a `VideoSync` into a drawable rectangle.
///
/// Returns `None` when the host must be hidden: `visible == false`, a
/// non-positive/invalid size or DPR, a degenerate client area, or a rectangle
/// that lies completely outside the client area after clamping.
pub fn resolve_placement(sync: &VideoSync, client_width: i32, client_height: i32) -> Option<PixelRect> {
    if !sync.visible {
        return None;
    }
    if client_width <= 0 || client_height <= 0 {
        return None;
    }
    if !sync.x.is_finite()
        || !sync.y.is_finite()
        || !sync.w.is_finite()
        || !sync.h.is_finite()
        || !sync.dpr.is_finite()
    {
        return None;
    }
    if !(sync.w > 0.0) || !(sync.h > 0.0) || !(sync.dpr > 0.0) {
        return None;
    }

    let left = round_to_i32(sync.x * sync.dpr);
    let right = round_to_i32((sync.x + sync.w) * sync.dpr);
    let top = round_to_i32(sync.y * sync.dpr);
    let bottom = round_to_i32((sync.y + sync.h) * sync.dpr);

    let left = left.clamp(0, client_width);
    let right = right.clamp(0, client_width);
    let top = top.clamp(0, client_height);
    let bottom = bottom.clamp(0, client_height);

    let width = right - left;
    let height = bottom - top;
    if width <= 0 || height <= 0 {
        return None;
    }

    Some(PixelRect {
        left,
        top,
        width,
        height,
    })
}

/// `f64::round` (half away from zero) with saturation instead of UB/wrapping
/// casts for out-of-range values.
fn round_to_i32(value: f64) -> i32 {
    let rounded = value.round();
    if rounded >= i32::MAX as f64 {
        i32::MAX
    } else if rounded <= i32::MIN as f64 {
        i32::MIN
    } else {
        rounded as i32
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn sync(visible: bool, x: f64, y: f64, w: f64, h: f64, dpr: f64) -> VideoSync {
        VideoSync {
            seq: 1,
            visible,
            x,
            y,
            w,
            h,
            dpr,
        }
    }

    #[test]
    fn scales_css_rect_by_dpr() {
        let rect = resolve_placement(&sync(true, 10.0, 20.0, 100.0, 50.0, 1.5), 1920, 1080).unwrap();
        assert_eq!(
            rect,
            PixelRect {
                left: 15,
                top: 30,
                width: 150,
                height: 75
            }
        );
        assert_eq!(rect.right(), 165);
        assert_eq!(rect.bottom(), 105);
    }

    #[test]
    fn rounds_both_edges_independently() {
        // left: round(0.6 * 1.25) = 1, right: round((0.6 + 120.4) * 1.25) = round(151.25) = 151
        let rect = resolve_placement(&sync(true, 0.6, 0.6, 120.4, 10.0, 1.25), 1920, 1080).unwrap();
        assert_eq!(rect.left, 1);
        assert_eq!(rect.right(), 151);
        assert_eq!(rect.width, 150);
        assert_eq!(rect.top, 1);
    }

    #[test]
    fn half_rounds_away_from_zero_like_win32_sizing() {
        // 0.5 * 1.0 rounds to 1 (not bank round to 0); 1.5 -> 2
        let rect = resolve_placement(&sync(true, 0.5, 1.5, 10.0, 10.0, 1.0), 1920, 1080).unwrap();
        assert_eq!((rect.left, rect.top), (1, 2));
        assert_eq!((rect.width, rect.height), (10, 10));
    }

    #[test]
    fn hidden_or_degenerate_rects_are_rejected() {
        assert!(resolve_placement(&sync(false, 0.0, 0.0, 100.0, 100.0, 1.0), 1920, 1080).is_none());
        assert!(resolve_placement(&sync(true, 0.0, 0.0, 0.0, 100.0, 1.0), 1920, 1080).is_none());
        assert!(resolve_placement(&sync(true, 0.0, 0.0, 100.0, 0.0, 1.0), 1920, 1080).is_none());
        assert!(resolve_placement(&sync(true, 0.0, 0.0, -100.0, 100.0, 1.0), 1920, 1080).is_none());
        assert!(resolve_placement(&sync(true, 0.0, 0.0, 100.0, 100.0, 0.0), 1920, 1080).is_none());
        assert!(resolve_placement(&sync(true, 0.0, 0.0, 100.0, 100.0, -1.0), 1920, 1080).is_none());
        assert!(resolve_placement(&sync(true, 0.0, 0.0, 100.0, 100.0, f64::NAN), 1920, 1080).is_none());
        assert!(resolve_placement(&sync(true, f64::INFINITY, 0.0, 100.0, 100.0, 1.0), 1920, 1080).is_none());
        assert!(resolve_placement(&sync(true, 0.0, 0.0, 100.0, 100.0, 1.0), 0, 1080).is_none());
        assert!(resolve_placement(&sync(true, 0.0, 0.0, 100.0, 100.0, 1.0), 1920, -1).is_none());
    }

    #[test]
    fn clamps_to_client_area() {
        // Partially off the left/top edge.
        let rect = resolve_placement(&sync(true, -50.0, -20.0, 100.0, 60.0, 1.0), 1920, 1080).unwrap();
        assert_eq!(rect, PixelRect { left: 0, top: 0, width: 50, height: 40 });

        // Larger than the client area: clamp right/bottom, keep origin.
        let rect = resolve_placement(&sync(true, 0.0, 0.0, 5000.0, 3000.0, 1.0), 1920, 1080).unwrap();
        assert_eq!(rect, PixelRect { left: 0, top: 0, width: 1920, height: 1080 });

        // Starts at the right edge -> fully clipped -> hide.
        assert!(resolve_placement(&sync(true, 1920.0, 0.0, 100.0, 100.0, 1.0), 1920, 1080).is_none());
        assert!(resolve_placement(&sync(true, 0.0, 1080.0, 100.0, 100.0, 1.0), 1920, 1080).is_none());
    }

    #[test]
    fn handles_huge_values_without_overflow() {
        let rect = resolve_placement(&sync(true, 1.0e300, 1.0e300, 1.0e300, 1.0e300, 2.0), 1920, 1080);
        assert!(rect.is_none(), "fully off-screen huge rect must be hidden");

        let rect = resolve_placement(&sync(true, 0.0, 0.0, 1.0e300, 1.0e300, 1.0), 1920, 1080).unwrap();
        assert_eq!(rect.width, 1920);
        assert_eq!(rect.height, 1080);
    }

    #[test]
    fn dpr_two_on_hidpi_client() {
        // 1280x720 CSS at 2.0 DPR inside a 2560x1440 client area.
        let rect = resolve_placement(&sync(true, 0.0, 0.0, 1280.0, 720.0, 2.0), 2560, 1440).unwrap();
        assert_eq!(rect, PixelRect { left: 0, top: 0, width: 2560, height: 1440 });
    }
}
