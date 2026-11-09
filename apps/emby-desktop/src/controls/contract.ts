// Wire contract between the transparent overlay window (`controls.html`) and
// the main window. The Rust side owns the window geometry and forwards events;
// every user intent travels back as an `overlay-action` payload so playback
// state (and Emby progress reporting) stays in the main window.

/** Overlay bar height in logical pixels (matches `overlay_set_expanded`). */
export const OVERLAY_BAR_HEIGHT = 64;
/** Expanded overlay height in logical pixels. */
export const OVERLAY_EXPANDED_HEIGHT = 320;
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
