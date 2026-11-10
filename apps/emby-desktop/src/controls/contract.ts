// Wire contract between the transparent overlay window (`controls.html`) and
// the main window. The Rust side owns the window geometry and forwards events;
// every user intent travels back as an `overlay-action` payload so playback
// state (and Emby progress reporting) stays in the main window.

import type { PlaybackMeta } from "../player/contract";

/**
 * Collapsed overlay window height in logical pixels. Keep in sync with
 * `COLLAPSED_HEIGHT` in `src-tauri/src/overlay.rs` and `--ov-bar-h` in
 * `overlay.css`.
 */
export const OVERLAY_BAR_HEIGHT = 96;
/**
 * Expanded overlay window height (bar + track popover) in logical pixels. Keep
 * in sync with `EXPANDED_HEIGHT` in `overlay.rs` and `--ov-panel-h` in
 * `overlay.css`. Rust shrinks it on windows shorter than this.
 */
export const OVERLAY_EXPANDED_HEIGHT = 352;
/** Expanded panel is the space above the bar. */
export const OVERLAY_PANEL_HEIGHT = OVERLAY_EXPANDED_HEIGHT - OVERLAY_BAR_HEIGHT;

export type OverlayAction =
  | { type: "toggle-play" }
  | { type: "seek"; seconds: number }
  | { type: "set-volume"; value: number }
  | { type: "toggle-mute" }
  | { type: "set-track"; kind: "audio" | "subtitle"; id: number | "no" }
  | { type: "toggle-fullscreen" }
  | { type: "back" }
  | { type: "retry" };

export type OverlayUiMode = "player" | "app";

/**
 * `overlay-meta` payload (main -> controls). `stamp` grows with every send so
 * the overlay can drop a late, out-of-order delivery; `meta: null` clears the
 * title row when the player page goes away.
 */
export interface OverlayMetaMessage {
  stamp: number;
  meta: PlaybackMeta | null;
}
