// Wire contract between the transparent overlay window (`controls.html`) and
// the main window. The Rust side owns the window geometry and forwards events;
// every user intent travels back as an `overlay-action` payload so playback
// state (and Emby progress reporting) stays in the main window.

import type { PlaybackMeta, PlayerSnapshot, PlayerStats } from "../player/contract";

/**
 * Collapsed overlay window height in logical pixels. The single source for the
 * overlay layout: `controls/main.tsx` publishes it as `--ov-bar-h`, so
 * `overlay.css` never repeats the number. It mirrors `COLLAPSED_HEIGHT` in
 * `src-tauri/src/overlay.rs`, which owns the actual window geometry.
 */
export const OVERLAY_BAR_HEIGHT = 96;
/**
 * Expanded overlay window height (bar + track popover) in logical pixels. The
 * Rust `EXPANDED_HEIGHT` mirror shrinks it on windows shorter than this.
 */
export const OVERLAY_EXPANDED_HEIGHT = 352;
/** Expanded panel is the space above the bar (`--ov-panel-h`). */
export const OVERLAY_PANEL_HEIGHT = OVERLAY_EXPANDED_HEIGHT - OVERLAY_BAR_HEIGHT;
/**
 * Top-left return/logo strip in logical pixels. The controls window covers the
 * whole video rectangle now; its window region exposes this corner plus the
 * bottom bar. Mirrors `TOP_BAND_HEIGHT` in `src-tauri/src/overlay.rs`.
 */
export const OVERLAY_TOP_BAND_HEIGHT = 88;
/** Widest the exposed top-left corner gets (the rest of the top edge is video). */
export const OVERLAY_TOP_BAND_MAX_WIDTH = 620;

export type OverlayAction =
  | { type: "toggle-play" }
  | { type: "previous" }
  | { type: "next" }
  | { type: "seek"; seconds: number }
  | { type: "set-volume"; value: number }
  | { type: "toggle-mute" }
  | { type: "set-track"; kind: "audio" | "subtitle"; id: number | "no" }
  | { type: "toggle-fullscreen" }
  | { type: "back" }
  | { type: "retry" };

export type OverlayUiMode = "player" | "app";

/**
 * `overlay-state` payload (main -> controls): the whole player surface in one
 * message, so the overlay never has to poll and can never show a mix of two
 * snapshots. `stamp` grows with every send and the overlay drops anything that
 * is not newer — a single ordering latch for snapshot, meta, stats and error.
 *
 * `null` means "no value for this field" (`snapshot` before the first
 * `player_subscribe` delivery, `meta` outside the player page, `stats` unless
 * the info panel asked for them, `error` and `notice` when clean).
 */
export interface OverlayState {
  stamp: number;
  snapshot: PlayerSnapshot | null;
  meta: PlaybackMeta | null;
  stats: PlayerStats | null;
  /** Playback failure the main window could not resolve (player_open, seek...). */
  error: string | null;
  /** Status line for states that are not failures (ended, auto-play pending). */
  notice: string | null;
}

/** What the player page hands to `publishOverlayState`; the stamp is added there. */
export type OverlayStateInput = Omit<OverlayState, "stamp">;

/**
 * `overlay-request` payload (controls -> main). Every request also makes the
 * main window resend the current state, so a freshly (re)loaded controls window
 * synchronises with one round trip.
 */
export interface OverlayRequest {
  /**
   * `true`/`false` starts/stops the live mpv statistics stream (the info panel is
   * open/closed); `null` leaves the stream alone and only asks for a resend.
   */
  stats: boolean | null;
}