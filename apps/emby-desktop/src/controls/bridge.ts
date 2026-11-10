import { invoke } from "@tauri-apps/api/core";
import { emitTo, listen } from "@tauri-apps/api/event";
import type { PlaybackMeta, PlayerSnapshot } from "../player/contract";
import type { OverlayAction, OverlayMetaMessage, OverlayUiMode } from "./contract";

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

// ---- title row (PlaybackMeta) ---------------------------------------------------

const META_EVENT = "overlay-meta";
const META_REQUEST_EVENT = "overlay-meta-request";
let lastMetaStamp = 0;

/** Grows across sends and across reloads of either window (wall clock based). */
function nextMetaStamp(): number {
  lastMetaStamp = Math.max(Date.now(), lastMetaStamp + 1);
  return lastMetaStamp;
}

/** Main window: show (or, with null, clear) the title row of the controls. */
export async function publishOverlayMeta(meta: PlaybackMeta | null): Promise<void> {
  const message: OverlayMetaMessage = { stamp: nextMetaStamp(), meta };
  await emitTo("controls", META_EVENT, message);
}

/** Main window: the controls window (re)loaded and asks for the title row. */
export async function onOverlayMetaRequest(handler: () => void): Promise<() => void> {
  return listen(META_REQUEST_EVENT, () => handler());
}

/** Overlay window: title-row updates from the player page. */
export async function onOverlayMeta(
  handler: (message: OverlayMetaMessage) => void,
): Promise<() => void> {
  return listen<OverlayMetaMessage>(META_EVENT, (event) => handler(event.payload));
}

/** Overlay window: ask the player page to resend the current title row. */
export async function requestOverlayMeta(): Promise<void> {
  await emitTo("main", META_REQUEST_EVENT, null);
}

// ---- browser-only preview ----------------------------------------------------------

/** `controls.html?preview=<state>` when opened with Vite, without Tauri. */
export type PreviewState = "playing" | "paused" | "loading" | "buffering" | "error";

const PREVIEW_STATES: readonly PreviewState[] = [
  "playing",
  "paused",
  "loading",
  "buffering",
  "error",
];

function previewParams(): URLSearchParams {
  if (typeof window === "undefined") return new URLSearchParams();
  return new URLSearchParams(window.location.search);
}

export function readPreviewState(): PreviewState {
  const value = previewParams().get("preview");
  return PREVIEW_STATES.find((state) => state === value) ?? "playing";
}

/**
 * Browser-only preview state so `controls.html` can be opened alone with Vite.
 * Real snapshots always come from `player_snapshot`.
 */
export function createPreviewSnapshot(
  state: PreviewState = readPreviewState(),
): PlayerSnapshot {
  const loading = state === "loading";
  return {
    seq: 1,
    status: state === "buffering" ? "playing" : state,
    positionSeconds: loading ? 0 : 21 * 60 + 47,
    durationSeconds: loading ? 0 : 44 * 60 + 12,
    volume: 72,
    muted: false,
    buffering: state === "buffering",
    videoWidth: loading ? 0 : 1920,
    videoHeight: loading ? 0 : 800,
    // mpv reports no tracks until the file is open.
    audioTracks: loading
      ? []
      : [
          { id: 1, type: "audio", title: "国语 · AAC 立体声", lang: "chi", selected: true },
          { id: 2, type: "audio", title: "English · Dolby Digital 5.1", lang: "eng", selected: false },
          { id: 3, type: "audio", title: "导演评论音轨", lang: "chi", selected: false },
          { id: 4, type: "audio", title: "", lang: "jpn", selected: false },
        ],
    subtitleTracks: loading
      ? []
      : [
          { id: 1, type: "subtitle", title: "简体中文", lang: "chi", selected: true },
          { id: 2, type: "subtitle", title: "繁體中文", lang: "chi", selected: false },
          { id: 3, type: "subtitle", title: "English (SDH)", lang: "eng", selected: false },
          { id: 4, type: "subtitle", title: "", lang: "jpn", selected: false },
        ],
    error: state === "error" ? "无法打开媒体：服务器返回 HTTP 404" : null,
  };
}

/** Preview title row; `?title=` / `?subtitle=` override it (empty clears). */
export function createPreviewMeta(): PlaybackMeta {
  const params = previewParams();
  return {
    title: params.get("title") ?? "长安夜话",
    subtitle: params.has("subtitle")
      ? params.get("subtitle") || null
      : "S1 · E8 — 上元灯会",
  };
}
