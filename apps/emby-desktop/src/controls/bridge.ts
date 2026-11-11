import { invoke } from "@tauri-apps/api/core";
import { emitTo, listen } from "@tauri-apps/api/event";
import type { PlaybackMeta, PlayerSnapshot, PlayerStats } from "../player/contract";
import type {
  OverlayAction,
  OverlayRequest,
  OverlayState,
  OverlayStateInput,
  OverlayUiMode,
} from "./contract";

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

// ---- unified player state (main -> controls) --------------------------------

const STATE_EVENT = "overlay-state";
const REQUEST_EVENT = "overlay-request";

let lastStateStamp = 0;

/** Grows across sends and across reloads of either window (wall clock based). */
function nextStateStamp(): number {
  lastStateStamp = Math.max(Date.now(), lastStateStamp + 1);
  return lastStateStamp;
}

/**
 * Main window: broadcast the whole player surface to the controls window. One
 * message carries snapshot, meta, stats and error, so the overlay keeps a
 * single ordering latch instead of merging three independent streams.
 */
export async function publishOverlayState(input: OverlayStateInput): Promise<void> {
  const message: OverlayState = { stamp: nextStateStamp(), ...input };
  await emitTo("controls", STATE_EVENT, message);
}

/** Overlay window: the single subscription for every piece of player state. */
export async function onOverlayState(
  handler: (state: OverlayState) => void,
): Promise<() => void> {
  return listen<OverlayState>(STATE_EVENT, (event) => handler(event.payload));
}

/**
 * Overlay window: ask the player page to resend the state, and to start/stop the
 * statistics stream. Sent on mount and whenever the info panel opens/closes.
 */
export async function requestOverlayState(request: OverlayRequest): Promise<void> {
  await emitTo("main", REQUEST_EVENT, request);
}

/** Main window: a controls window (re)loaded, or its stats demand changed. */
export async function onOverlayRequest(
  handler: (request: OverlayRequest) => void,
): Promise<() => void> {
  return listen<OverlayRequest>(REQUEST_EVENT, (event) => handler(event.payload));
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
 * Real states always arrive through `overlay-state`.
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
    chapters: [
      { seconds: 0, title: "开场" },
      { seconds: 420, title: "第一幕" },
      { seconds: 980, title: "第二幕" },
      { seconds: 1500, title: "第三幕" },
      { seconds: 2100, title: "结尾" },
    ],
  };
}

/** Preview statistics for the info panel (the real ones come from mpv). */
export function createPreviewStats(): PlayerStats {
  return {
    hwdec: "d3d11va",
    vo: "gpu-next",
    pixelFormat: "p010",
    videoCodec: "hevc (Main 10)",
    displayFps: 59.94,
    videoFps: 23.976,
    droppedFrames: 0,
    bitrate: 18_400_000,
  };
}