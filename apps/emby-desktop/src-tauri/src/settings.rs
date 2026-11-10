//! Persisted application settings.
//!
//! The settings file lives next to `mpv.conf` in the Tauri app config directory
//! (`%APPDATA%\com.ember.emby-desktop\settings.json` on Windows). It only
//! contains local preferences, never credentials.
//!
//! A missing, unreadable or corrupt file falls back to [`AppSettings::default`],
//! whose quality preset is the historical low-overhead `profile=fast` so an
//! upgrade does not silently change playback quality.

use std::fs;
use std::io;
use std::path::PathBuf;

use serde::{Deserialize, Serialize};
use tauri::State;

use crate::player::PlayerService;

pub const SETTINGS_FILE: &str = "settings.json";
pub const DEFAULT_PAGE_SIZE: u32 = 120;

/// Player quality preset exposed to the settings page.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum VideoQuality {
    /// mpv's own defaults; no quality profile is forced.
    Default,
    /// Low-overhead preset: `profile=fast` (the old hard-coded behaviour).
    Low,
}

impl Default for VideoQuality {
    fn default() -> Self {
        // Preserve the pre-settings behaviour on first run / broken file.
        Self::Low
    }
}

impl VideoQuality {
    /// mpv profile name, if this preset forces one.
    pub fn mpv_profile(self) -> Option<&'static str> {
        match self {
            Self::Default => None,
            Self::Low => Some("fast"),
        }
    }
}

/// Settings shared with the frontend through the `settings_*` commands.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", default)]
pub struct AppSettings {
    pub video_quality: VideoQuality,
    pub auto_play: bool,
    pub page_size: u32,
}

impl Default for AppSettings {
    fn default() -> Self {
        Self {
            video_quality: VideoQuality::default(),
            auto_play: true,
            page_size: DEFAULT_PAGE_SIZE,
        }
    }
}

impl AppSettings {
    /// Replaces nonsensical values left by old or hand-edited JSON.
    pub fn normalized(mut self) -> Self {
        if self.page_size == 0 {
            self.page_size = DEFAULT_PAGE_SIZE;
        }
        self
    }
}

/// JSON reader/writer for the app config directory.
#[derive(Debug, Clone)]
pub struct SettingsStore {
    dir: PathBuf,
}

impl SettingsStore {
    pub fn new(dir: impl Into<PathBuf>) -> Self {
        Self { dir: dir.into() }
    }

    pub fn path(&self) -> PathBuf {
        self.dir.join(SETTINGS_FILE)
    }

    /// Reads the settings file. Missing or corrupt files yield the defaults
    /// instead of blocking playback; I/O errors are reported and also fall back
    /// so the app stays usable.
    pub fn load(&self) -> AppSettings {
        let path = self.path();
        let bytes = match fs::read(&path) {
            Ok(bytes) => bytes,
            Err(err) if err.kind() == io::ErrorKind::NotFound => {
                return AppSettings::default();
            }
            Err(err) => {
                eprintln!("[settings] could not read {}: {err}", path.display());
                return AppSettings::default();
            }
        };
        match serde_json::from_slice::<AppSettings>(&bytes) {
            Ok(settings) => settings.normalized(),
            Err(err) => {
                eprintln!(
                    "[settings] {} is corrupt ({err}); using defaults",
                    path.display()
                );
                AppSettings::default()
            }
        }
    }

    /// Writes the settings file, creating the config directory if needed.
    pub fn save(&self, settings: &AppSettings) -> io::Result<()> {
        fs::create_dir_all(&self.dir)?;
        let bytes = serde_json::to_vec_pretty(settings)
            .map_err(|err| io::Error::new(io::ErrorKind::InvalidData, err))?;
        fs::write(self.path(), bytes)
    }
}

/// Reads the persisted settings (defaults when the file is missing/corrupt).
#[tauri::command]
pub fn settings_get(state: State<'_, PlayerService>) -> Result<AppSettings, String> {
    Ok(state.current_settings())
}

/// Persists settings and returns the normalized value the app now uses.
#[tauri::command]
pub fn settings_set(
    settings: AppSettings,
    state: State<'_, PlayerService>,
) -> Result<AppSettings, String> {
    state.save_settings(settings)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;
    use std::path::PathBuf;
    use std::sync::atomic::{AtomicU64, Ordering};

    fn temp_store(label: &str) -> (SettingsStore, PathBuf) {
        static COUNTER: AtomicU64 = AtomicU64::new(0);
        let id = COUNTER.fetch_add(1, Ordering::SeqCst);
        let dir = std::env::temp_dir().join(format!(
            "emby-desktop-settings-{}-{label}-{id}",
            std::process::id()
        ));
        (SettingsStore::new(dir.clone()), dir)
    }

    #[test]
    fn defaults_are_low_overhead_and_auto_play_on() {
        let settings = AppSettings::default();
        assert_eq!(settings.video_quality, VideoQuality::Low);
        assert_eq!(settings.video_quality.mpv_profile(), Some("fast"));
        assert!(settings.auto_play);
        assert_eq!(settings.page_size, DEFAULT_PAGE_SIZE);
        assert_eq!(VideoQuality::Default.mpv_profile(), None);
    }

    #[test]
    fn missing_file_falls_back_to_defaults() {
        let (store, dir) = temp_store("missing");
        assert_eq!(store.load(), AppSettings::default());
        let _ = fs::remove_dir_all(dir);
    }

    #[test]
    fn round_trip_persists_camel_case_values() {
        let (store, dir) = temp_store("roundtrip");
        let settings = AppSettings {
            video_quality: VideoQuality::Default,
            auto_play: false,
            page_size: 60,
        };
        store.save(&settings).expect("save settings");
        assert_eq!(store.load(), settings);

        let text = fs::read_to_string(store.path()).expect("read settings file");
        assert!(text.contains("\"videoQuality\": \"default\""), "{text}");
        assert!(text.contains("\"autoPlay\": false"), "{text}");
        assert!(text.contains("\"pageSize\": 60"), "{text}");
        let _ = fs::remove_dir_all(dir);
    }

    #[test]
    fn corrupt_file_falls_back_to_defaults() {
        let (store, dir) = temp_store("corrupt");
        fs::create_dir_all(&dir).expect("create temp dir");
        fs::write(store.path(), "{not valid json").expect("write corrupt file");
        assert_eq!(store.load(), AppSettings::default());
        let _ = fs::remove_dir_all(dir);
    }

    #[test]
    fn partial_file_uses_defaults_for_missing_fields() {
        let (store, dir) = temp_store("partial");
        fs::create_dir_all(&dir).expect("create temp dir");
        fs::write(store.path(), r#"{"autoPlay":false}"#).expect("write partial file");
        let loaded = store.load();
        assert!(!loaded.auto_play);
        assert_eq!(loaded.video_quality, VideoQuality::Low);
        assert_eq!(loaded.page_size, DEFAULT_PAGE_SIZE);
        let _ = fs::remove_dir_all(dir);
    }
}
