import { invoke } from "@tauri-apps/api/core";
import type {
  EmbyItem,
  EmbyItemsPage,
  EmbyItemsRequest,
  EmbyPersonItemsRequest,
  EmbyPlaybackTarget,
  EmbyResumeRequest,
  EmbySession,
  EmbySetFavoriteRequest,
  EmbySetPlayedRequest,
  EmbyView,
} from "./contract";

export type {
  EmbyItemsRequest,
  EmbyPersonItemsRequest,
  EmbySetFavoriteRequest,
  EmbySetPlayedRequest,
} from "./contract";

export type EmbyPlaybackEvent =
  | "start"
  | "progress"
  | "pause"
  | "unpause"
  | "seek"
  | "stop"
  | "ended";

export interface EmbyCredentials {
  server: string;
  username: string;
  password: string;
}

export interface EmbyImageRequest {
  itemId: string;
  imageType?: string;
  maxWidth?: number;
  tag?: string;
}

export interface EmbyPlaybackReportRequest {
  itemId: string;
  mediaSourceId?: string;
  playSessionId?: string;
  positionSeconds: number;
  paused: boolean;
  event: EmbyPlaybackEvent;
}

type TauriAwareWindow = Window & { __TAURI_INTERNALS__?: unknown };

export function hasTauriRuntime(): boolean {
  if (typeof window === "undefined") return false;
  return Boolean((window as TauriAwareWindow).__TAURI_INTERNALS__);
}

function requireDesktop(): void {
  if (!hasTauriRuntime()) {
    throw new Error("需要 Tauri 桌面环境才能连接 Emby。");
  }
}

export function describeEmbyError(error: unknown, fallback: string): string {
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

  if (
    normalized.includes("transcod") ||
    normalized.includes("转码") ||
    normalized.includes("no compatible stream") ||
    normalized.includes("direct play is not possible")
  ) {
    return "该条目需要转码，当前不支持直接播放。";
  }
  if (
    normalized.includes("401") ||
    normalized.includes("unauthorized") ||
    normalized.includes("invalid credentials") ||
    normalized.includes("invalid username") ||
    normalized.includes("authentication failed") ||
    normalized.includes("access denied") ||
    normalized.includes("登录已失效")
  ) {
    return "用户名或密码不正确，或登录已失效。";
  }
  if (
    normalized.includes("timeout") ||
    normalized.includes("timed out") ||
    normalized.includes("超时")
  ) {
    return "连接 Emby 超时，请检查服务器地址与网络。";
  }
  if (
    normalized.includes("connection refused") ||
    normalized.includes("failed to connect") ||
    normalized.includes("connect error") ||
    normalized.includes("network error") ||
    normalized.includes("error sending request") ||
    normalized.includes("dns") ||
    normalized.includes("unreachable") ||
    normalized.includes("failed to fetch") ||
    normalized.includes("无法连接") ||
    normalized.includes("网络请求失败")
  ) {
    return "无法连接 Emby 服务器，请检查地址、端口与网络。";
  }
  if (
    normalized.includes("unknown command") ||
    normalized.includes("not implemented") ||
    (normalized.includes("command") && normalized.includes("not found")) ||
    normalized.includes("not available")
  ) {
    return `Emby 后端尚未就绪（${message}）。`;
  }
  return message;
}

export async function embyLogin(req: EmbyCredentials): Promise<EmbySession> {
  requireDesktop();
  return invoke<EmbySession>("emby_login", { req });
}

export async function embyLogout(): Promise<void> {
  requireDesktop();
  await invoke<void>("emby_logout");
}

export async function embySession(): Promise<EmbySession | null> {
  requireDesktop();
  return invoke<EmbySession | null>("emby_session");
}

export async function embyViews(): Promise<EmbyView[]> {
  requireDesktop();
  return invoke<EmbyView[]>("emby_views");
}

export async function embyItems(req: EmbyItemsRequest): Promise<EmbyItemsPage> {
  requireDesktop();
  return invoke<EmbyItemsPage>("emby_items", { req });
}

export async function embyResume(req: EmbyResumeRequest = {}): Promise<EmbyItemsPage> {
  requireDesktop();
  return invoke<EmbyItemsPage>("emby_resume", { req });
}

export async function embyItem(id: string): Promise<EmbyItem> {
  requireDesktop();
  return invoke<EmbyItem>("emby_item", { id });
}

/** "More like this" for a movie or series. */
export async function embySimilar(id: string, limit?: number): Promise<EmbyItemsPage> {
  requireDesktop();
  return invoke<EmbyItemsPage>("emby_similar", { id, ...(limit ? { limit } : {}) });
}

/** Works featuring a person (actor/director). */
export async function embyPersonItems(req: EmbyPersonItemsRequest): Promise<EmbyItemsPage>;
export async function embyPersonItems(personId: string, limit?: number): Promise<EmbyItemsPage>;
export async function embyPersonItems(
  personIdOrRequest: string | EmbyPersonItemsRequest,
  limit?: number,
): Promise<EmbyItemsPage> {
  requireDesktop();
  const request =
    typeof personIdOrRequest === "string"
      ? { personId: personIdOrRequest, ...(limit ? { limit } : {}) }
      : personIdOrRequest;
  return invoke<EmbyItemsPage>("emby_person_items", {
    personId: request.personId,
    ...(request.limit ? { limit: request.limit } : {}),
  });
}

/** Marks an item watched or unwatched for the signed-in user. */
export async function embySetPlayed(req: EmbySetPlayedRequest): Promise<void>;
export async function embySetPlayed(itemId: string, played: boolean): Promise<void>;
export async function embySetPlayed(
  itemIdOrRequest: string | EmbySetPlayedRequest,
  played?: boolean,
): Promise<void> {
  requireDesktop();
  const request =
    typeof itemIdOrRequest === "string"
      ? { itemId: itemIdOrRequest, played: played === true }
      : itemIdOrRequest;
  await invoke<void>("emby_set_played", { id: request.itemId, played: request.played });
}

/** Adds or removes the signed-in user's favorite mark. */
export async function embySetFavorite(req: EmbySetFavoriteRequest): Promise<void>;
export async function embySetFavorite(itemId: string, favorite: boolean): Promise<void>;
export async function embySetFavorite(
  itemIdOrRequest: string | EmbySetFavoriteRequest,
  favorite?: boolean,
): Promise<void> {
  requireDesktop();
  const request =
    typeof itemIdOrRequest === "string"
      ? { itemId: itemIdOrRequest, favorite: favorite === true }
      : itemIdOrRequest;
  await invoke<void>("emby_set_favorite", {
    id: request.itemId,
    favorite: request.favorite,
  });
}

export async function embyPlaybackTarget(itemId: string): Promise<EmbyPlaybackTarget> {
  requireDesktop();
  return invoke<EmbyPlaybackTarget>("emby_playback_target", { req: { itemId } });
}

export async function embyReportPlayback(req: EmbyPlaybackReportRequest): Promise<void> {
  requireDesktop();
  await invoke<void>("emby_report_playback", { req });
}

export async function embyImageUrl(req: EmbyImageRequest): Promise<string> {
  requireDesktop();
  return invoke<string>("emby_image_url", { req });
}
