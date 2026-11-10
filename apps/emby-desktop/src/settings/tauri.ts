import { invoke } from "@tauri-apps/api/core";
import { hasTauriRuntime } from "../emby/client";

/** Player quality preset; matches the Rust `VideoQuality` enum. */
export type VideoQuality = "default" | "low";

/** Persisted application settings, shared with the Rust `settings_*` commands. */
export interface AppSettings {
  videoQuality: VideoQuality;
  autoPlay: boolean;
  pageSize: number;
}

/**
 * Defaults match the Rust side: the first run / a broken settings file keeps
 * the historical low-overhead `profile=fast` behaviour and auto-play on.
 */
export const DEFAULT_APP_SETTINGS: AppSettings = {
  videoQuality: "low",
  autoPlay: true,
  pageSize: 120,
};

export function hasSettingsRuntime(): boolean {
  return hasTauriRuntime();
}

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
