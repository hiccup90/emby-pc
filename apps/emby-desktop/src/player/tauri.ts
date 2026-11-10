import { Channel, invoke } from "@tauri-apps/api/core";
import type {
  OpenRequest,
  PlayerSnapshot,
  PlayerStats,
  PlayerTrack,
  VideoSync,
} from "./contract";

type TauriAwareWindow = Window & { __TAURI_INTERNALS__?: unknown };

export function hasTauriRuntime(): boolean {
  if (typeof window === "undefined") return false;
  return Boolean((window as TauriAwareWindow).__TAURI_INTERNALS__);
}

export function describeError(error: unknown, fallback: string): string {
  let message = "";
  if (typeof error === "string") {
    message = error;
  } else if (error instanceof Error) {
    message = error.message;
  } else if (typeof error === "object" && error !== null && "message" in error) {
    const value = (error as { message?: unknown }).message;
    if (typeof value === "string") message = value;
  }

  message = message.trim();
  if (!message) return fallback;

  const normalized = message.toLowerCase();
  const backendUnavailable =
    normalized.includes("unknown command") ||
    normalized.includes("not implemented") ||
    (normalized.includes("command") && normalized.includes("not found")) ||
    normalized.includes("not available");

  if (normalized.includes("native video host is unavailable") || normalized.includes("windows-only")) {
    return "播放器组件不可用：原生视频嵌入仅支持 Windows。";
  }
  if (normalized.includes("failed to load") || normalized.includes("cannot load") || normalized.includes("libmpv")) {
    return `播放器组件缺失或无法加载：${message}`;
  }
  if (backendUnavailable) {
    return `${fallback}：播放器后端尚未就绪（${message}）`;
  }
  if (normalized.includes("invalid url") || normalized.includes("failed to parse url")) {
    return "播放地址无效，请检查 URL 后重试。";
  }
  if (normalized.includes("empty url")) {
    return "请输入播放地址。";
  }
  if (normalized.includes("unknown track kind")) {
    return "无法切换轨道：轨道类型无效。";
  }
  if (normalized.includes("not open") || normalized.includes("no media")) {
    return "播放器尚未载入媒体。";
  }
  return message;
}

export async function invokeVideoSync(req: VideoSync): Promise<void> {
  await invoke<void>("video_sync", { req });
}

export async function invokePlayerOpen(req: OpenRequest): Promise<PlayerSnapshot> {
  return invoke<PlayerSnapshot>("player_open", { req });
}

export async function invokePlayerSetPaused(paused: boolean): Promise<void> {
  await invoke<void>("player_set_paused", { paused });
}

export async function invokePlayerSeek(seconds: number, exact: boolean): Promise<void> {
  await invoke<void>("player_seek", { seconds, exact });
}

export async function invokePlayerSetVolume(volume: number): Promise<void> {
  await invoke<void>("player_set_volume", { volume });
}

export async function invokePlayerSetMuted(muted: boolean): Promise<void> {
  await invoke<void>("player_set_muted", { muted });
}

export async function invokePlayerSetTrack(kind: PlayerTrack["type"], id: number): Promise<void> {
  await invoke<void>("player_set_track", { kind, id });
}

export async function invokePlayerStop(): Promise<void> {
  await invoke<void>("player_stop");
}

export async function invokePlayerSnapshot(): Promise<PlayerSnapshot> {
  return invoke<PlayerSnapshot>("player_snapshot");
}

export async function invokePlayerStats(): Promise<PlayerStats> {
  return invoke<PlayerStats>("player_stats");
}

export async function invokePlayerToggleFullscreen(): Promise<boolean> {
  return invoke<boolean>("player_toggle_fullscreen");
}

export function createPlayerChannel(onEvent: (snapshot: PlayerSnapshot) => void): Channel<PlayerSnapshot> {
  const channel = new Channel<PlayerSnapshot>();
  channel.onmessage = onEvent;
  return channel;
}

export async function invokePlayerSubscribe(channel: Channel<PlayerSnapshot>): Promise<void> {
  await invoke<void>("player_subscribe", { onEvent: channel });
}
