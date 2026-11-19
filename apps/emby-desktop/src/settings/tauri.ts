import { invoke } from "@tauri-apps/api/core";
import { hasTauriRuntime } from "../emby/client";
import type { EmbyLibraryQuery } from "../emby/useEmby";

/** Player quality preset; matches the Rust `VideoQuality` enum. */
export type VideoQuality = "default" | "low";

/** Card shape for one library; matches the Rust `LibraryViewMode` enum. */
export type LibraryViewMode = "poster" | "thumbnail";

/** Persisted application settings, shared with the Rust `settings_*` commands. */
export interface AppSettings {
  videoQuality: VideoQuality;
  autoPlay: boolean;
  pageSize: number;
  /** Per Emby view id; absent means the library's own default. */
  libraryViewModes: Record<string, LibraryViewMode>;
  /** Per Emby view id; absent means the global library query defaults. */
  libraryQueries: Record<string, EmbyLibraryQuery>;
  /**
   * Ordered Emby view ids shown on the home page. `null` means the default
   * movie/TV libraries; an explicit list (even empty) is the user's choice.
   */
  homeViewIds: string[] | null;
}

/**
 * Defaults match the Rust side: the first run / a broken settings file keeps
 * the historical low-overhead `profile=fast` behaviour and auto-play on.
 */
export const DEFAULT_APP_SETTINGS: AppSettings = {
  videoQuality: "low",
  autoPlay: true,
  pageSize: 120,
  libraryViewModes: {},
  libraryQueries: {},
  homeViewIds: null,
};

/** Reads the persisted settings; browser previews fall back to defaults. */
export async function getSettings(): Promise<AppSettings> {
  if (!hasTauriRuntime()) return { ...DEFAULT_APP_SETTINGS };
  return invoke<AppSettings>("settings_get");
}

/** Persists settings and returns the normalized value stored by the backend. */
export async function saveSettings(settings: AppSettings): Promise<AppSettings> {
  if (!hasTauriRuntime()) {
    throw new Error("需要 Tauri 桌面环境才能保存设置。");
  }
  return invoke<AppSettings>("settings_set", { settings });
}
