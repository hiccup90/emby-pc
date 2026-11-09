import { invoke } from "@tauri-apps/api/core";
import { emitTo, listen } from "@tauri-apps/api/event";
import type { PlayerSnapshot } from "../player/contract";
import type { OverlayAction, OverlayUiMode } from "./contract";

type TauriAwareWindow = Window & { __TAURI_INTERNALS__?: unknown };

export function hasTauriRuntime(): boolean {
  if (typeof window === "undefined") return false;
  return Boolean((window as TauriAwareWindow).__TAURI_INTERNALS__);
}

/**
 * Main window: enter/leave the borderless player mode. Idempotent on the Rust
 * side so StrictMode's mount/unmount/mount cycle is harmless.
 */
export async function invokePlayerUiMode(mode: OverlayUiMode): Promise<void> {
  await invoke<void>("player_ui_mode", { mode });
}

/** Overlay window: grow to the expanded panel height (bottom edge anchored). */
export async function setOverlayExpanded(expanded: boolean): Promise<void> {
  await invoke<void>("overlay_set_expanded", { expanded });
}

/** Overlay window: when false the cursor passes through to the video. */
export async function setOverlayInteractive(interactive: boolean): Promise<void> {
  await invoke<void>("overlay_set_interactive", { interactive });
}

/** Overlay window: start moving the main window from a blank bar area. */
export async function dragOverlayWindow(): Promise<void> {
  await invoke<void>("overlay_drag_window");
}

/** Overlay window: hand a player intent to the main window. */
export async function sendOverlayAction(action: OverlayAction): Promise<void> {
  await emitTo("main", "overlay-action", action);
}

/** Overlay window: Rust forwards cursor activity over the video as a wake. */
export async function onOverlayWake(handler: () => void): Promise<() => void> {
  return listen("overlay-wake", () => handler());
}

/** Overlay window: poll the shared snapshot (no Channel subscription here). */
export async function fetchPlayerSnapshot(): Promise<PlayerSnapshot> {
  return invoke<PlayerSnapshot>("player_snapshot");
}

/**
 * Browser-only preview state so `controls.html` can be opened alone with Vite.
 * Real snapshots always come from `player_snapshot`.
 */
export function createPreviewSnapshot(): PlayerSnapshot {
  return {
    seq: 1,
    status: "playing",
    positionSeconds: 132,
    durationSeconds: 96 * 60,
    volume: 72,
    muted: false,
    buffering: false,
    videoWidth: 1920,
    videoHeight: 1080,
    audioTracks: [
      { id: 1, type: "audio", title: "国语 · 立体声", lang: "zh", selected: true },
      { id: 2, type: "audio", title: "原声 · 5.1", lang: "en", selected: false },
    ],
    subtitleTracks: [
      { id: 1, type: "subtitle", title: "简体中文", lang: "zh", selected: true },
      { id: 2, type: "subtitle", title: "English", lang: "en", selected: false },
    ],
    error: null,
  };
}
