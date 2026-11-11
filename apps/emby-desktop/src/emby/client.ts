import { invoke } from "@tauri-apps/api/core";
import { listen } from "@tauri-apps/api/event";
import { EMBY_SESSION_EVENT } from "./contract";
import type {
  EmbyItem,
  EmbyItemsPage,
  EmbyItemsRequest,
  EmbyPersonItemsRequest,
  EmbyPlaybackTarget,
  EmbyResumeRequest,
  EmbySession,
  EmbySessionBoot,
  EmbySessionReason,
  EmbySessionEvent,
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

interface EmbyImageRequest {
  itemId: string;
  imageType?: string;
  maxWidth?: number;
  tag?: string;
}

interface EmbyPlaybackReportRequest {
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

function rawEmbyErrorMessage(error: unknown): string {
  if (typeof error === "string") return error;
  if (error instanceof Error) return error.message;
  if (typeof error === "object" && error !== null && "message" in error) {
    const value = (error as { message?: unknown }).message;
    if (typeof value === "string") return value;
  }
  return "";
}

/**
 * Text for every machine-readable reason the backend can send. `null` means
 * "no better wording than the backend own message": the class is known but the
 * detail lives in the message (a 404, a plain 5xx, an invalid request).
 */
const REASON_TEXT: Record<EmbySessionReason, string | null> = {
  none: null,
  token_rejected: "登录已失效，请重新登录。",
  invalid_credentials: "用户名或密码不正确。",
  unreachable: "无法连接 Emby 服务器，请检查地址、端口与网络。",
  storage_error: "无法读写本地登录信息，请检查磁盘权限。",
  invalid_request: null,
  server_error: null,
};

function embyErrorReason(error: unknown): EmbySessionReason | null {
  if (typeof error !== "object" || error === null) return null;
  const value = (error as { reason?: unknown }).reason;
  return typeof value === "string" && value in REASON_TEXT ? (value as EmbySessionReason) : null;
}

/**
 * One short line for a rejected `emby_*` call.
 *
 * The backend classifies every failure (ApiErrorKind -> EmbyCommandError.reason),
 * so this maps that reason and only falls back to the message text: the UI never
 * re-parses HTTP status codes or English wording. Errors raised before IPC (no
 * Tauri runtime, local guards) carry no reason and keep their own message.
 */
export function describeEmbyError(error: unknown, fallback: string): string {
  const message = rawEmbyErrorMessage(error).trim();
  const reason = embyErrorReason(error);
  if (reason) return REASON_TEXT[reason] ?? message ?? fallback;

  const normalized = message.toLowerCase();
  const backendMissing =
    normalized.includes("unknown command") ||
    normalized.includes("not implemented") ||
    (normalized.includes("command") && normalized.includes("not found")) ||
    normalized.includes("not available");
  if (backendMissing) return "Emby 后端尚未就绪（" + message + "）。";
  return message || fallback;
}
async function invokeEmby<T>(command: string, args?: Record<string, unknown>): Promise<T> {
  return invoke<T>(command, args);
}

export async function embyLogin(req: EmbyCredentials): Promise<EmbySession> {
  requireDesktop();
  return invokeEmby<EmbySession>("emby_login", { req });
}

export async function embyLogout(): Promise<EmbySessionBoot> {
  requireDesktop();
  return invokeEmby<EmbySessionBoot>("emby_logout");
}

/** Single-flight startup restore; pass force=true only for a user retry. */
export async function embySessionBoot(force = false): Promise<EmbySessionBoot> {
  requireDesktop();
  return invokeEmby<EmbySessionBoot>("emby_session_boot", force ? { force: true } : undefined);
}

/** Subscribe to the backend's signed-out event (running 401). */
export function onEmbySessionEvent(
  handler: (event: EmbySessionEvent) => void,
): Promise<() => void> {
  return listen<EmbySessionEvent>(EMBY_SESSION_EVENT, (event) => handler(event.payload));
}

export async function embyViews(): Promise<EmbyView[]> {
  requireDesktop();
  return invokeEmby<EmbyView[]>("emby_views");
}

export async function embyItems(req: EmbyItemsRequest): Promise<EmbyItemsPage> {
  requireDesktop();
  return invokeEmby<EmbyItemsPage>("emby_items", { req });
}

export async function embyResume(req: EmbyResumeRequest = {}): Promise<EmbyItemsPage> {
  requireDesktop();
  return invokeEmby<EmbyItemsPage>("emby_resume", { req });
}

export async function embyItem(id: string): Promise<EmbyItem> {
  requireDesktop();
  return invokeEmby<EmbyItem>("emby_item", { id });
}

/** "More like this" for a movie or series. */
export async function embySimilar(id: string, limit?: number): Promise<EmbyItemsPage> {
  requireDesktop();
  return invokeEmby<EmbyItemsPage>("emby_similar", { id, ...(limit ? { limit } : {}) });
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
  return invokeEmby<EmbyItemsPage>("emby_person_items", {
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
  await invokeEmby<void>("emby_set_played", { id: request.itemId, played: request.played });
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
  await invokeEmby<void>("emby_set_favorite", {
    id: request.itemId,
    favorite: request.favorite,
  });
}

export async function embyPlaybackTarget(itemId: string): Promise<EmbyPlaybackTarget> {
  requireDesktop();
  return invokeEmby<EmbyPlaybackTarget>("emby_playback_target", { req: { itemId } });
}

export async function embyReportPlayback(req: EmbyPlaybackReportRequest): Promise<void> {
  requireDesktop();
  await invokeEmby<void>("emby_report_playback", { req });
}

export async function embyImageUrl(req: EmbyImageRequest): Promise<string> {
  requireDesktop();
  return invokeEmby<string>("emby_image_url", { req });
}