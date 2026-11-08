//! Session and device-id persistence.
//!
//! The app hands in the Tauri `app_config_dir()`; this module owns the two
//! files inside it:
//! - `session.json`: server URL, access token, user/server names, device id.
//!   Removed by `emby_logout` and on 401/403.
//! - `device.json`: the Emby `DeviceId`. It survives logout so the server keeps
//!   associating the client with the same device.
//!
//! Writes are atomic (temp file + rename) and, on Unix, `0600`. There is no
//! OS-level encryption here: on Windows the token file lives under the user's
//! roaming app-data directory, which is protected by the user profile ACL.
//! A failure to write is reported, never silently ignored.

use std::fmt;
use std::fs::{self, OpenOptions};
use std::io::{self, Write};
use std::path::{Path, PathBuf};
use std::time::{SystemTime, UNIX_EPOCH};

use serde::{Deserialize, Serialize};

use crate::error::{ApiError, ApiResult};
use crate::model::EmbySession;

pub const SESSION_FILE: &str = "session.json";
pub const DEVICE_FILE: &str = "device.json";

/// Persisted login state. `Debug` is implemented manually so the token cannot
/// leak through an accidental `{:?}`.
#[derive(Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct StoredSession {
    pub server_url: String,
    pub access_token: String,
    pub user_id: String,
    pub user_name: String,
    pub server_name: String,
    pub device_id: String,
}

impl StoredSession {
    /// Frontend-facing view: everything except the access token.
    pub fn public(&self) -> EmbySession {
        EmbySession {
            server_url: self.server_url.clone(),
            user_id: self.user_id.clone(),
            user_name: self.user_name.clone(),
            server_name: self.server_name.clone(),
            device_id: self.device_id.clone(),
        }
    }
}

impl fmt::Debug for StoredSession {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("StoredSession")
            .field("server_url", &self.server_url)
            .field("access_token", &"<redacted>")
            .field("user_id", &self.user_id)
            .field("user_name", &self.user_name)
            .field("server_name", &self.server_name)
            .field("device_id", &self.device_id)
            .finish()
    }
}

#[derive(Debug, Default, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
struct DeviceFile {
    device_id: String,
}

/// Reader/writer for `session.json` and `device.json` in a config directory.
#[derive(Debug, Clone)]
pub struct SessionStore {
    dir: PathBuf,
}

impl SessionStore {
    pub fn new(dir: impl Into<PathBuf>) -> Self {
        Self { dir: dir.into() }
    }

    pub fn dir(&self) -> &Path {
        &self.dir
    }

    pub fn session_path(&self) -> PathBuf {
        self.dir.join(SESSION_FILE)
    }

    pub fn device_path(&self) -> PathBuf {
        self.dir.join(DEVICE_FILE)
    }

    /// Reads the stored session. A missing file, a corrupt file or a file with
    /// empty mandatory fields all mean "not logged in" (`Ok(None)`); real I/O
    /// errors (permissions) are reported.
    pub fn load(&self) -> ApiResult<Option<StoredSession>> {
        let path = self.session_path();
        let bytes = match fs::read(&path) {
            Ok(bytes) => bytes,
            Err(err) if err.kind() == io::ErrorKind::NotFound => return Ok(None),
            Err(err) => {
                return Err(ApiError::config(format!(
                    "无法读取会话文件 {}：{err}",
                    path.display()
                )))
            }
        };
        let Ok(session) = serde_json::from_slice::<StoredSession>(&bytes) else {
            // Old or truncated file: treat as logged out; a fresh login rewrites it.
            return Ok(None);
        };
        if session.access_token.trim().is_empty()
            || session.server_url.trim().is_empty()
            || session.user_id.trim().is_empty()
        {
            return Ok(None);
        }
        Ok(Some(session))
    }

    pub fn save(&self, session: &StoredSession) -> ApiResult<()> {
        self.ensure_dir()?;
        let bytes = serde_json::to_vec_pretty(session)
            .map_err(|_| ApiError::config("无法序列化会话数据"))?;
        let path = self.session_path();
        write_private(&path, &bytes).map_err(|err| {
            ApiError::config(format!("无法写入会话文件 {}：{err}", path.display()))
        })
    }

    /// Removes `session.json`. Missing file is not an error.
    pub fn clear(&self) -> ApiResult<()> {
        let path = self.session_path();
        match fs::remove_file(&path) {
            Ok(()) => Ok(()),
            Err(err) if err.kind() == io::ErrorKind::NotFound => Ok(()),
            Err(err) => Err(ApiError::config(format!(
                "无法删除会话文件 {}：{err}",
                path.display()
            ))),
        }
    }

    /// Returns the persisted device id, creating it on first use.
    ///
    /// Migration order: `device.json`, then an existing `session.json` (older
    /// builds wrote the id there), then a freshly generated id.
    pub fn device_id(&self) -> ApiResult<String> {
        if let Some(device_id) = self.read_device_id()? {
            return Ok(device_id);
        }
        if let Some(session) = self.load()? {
            let device_id = session.device_id.trim().to_string();
            if !device_id.is_empty() {
                self.save_device_id(&device_id)?;
                return Ok(device_id);
            }
        }
        let device_id = generate_device_id();
        self.save_device_id(&device_id)?;
        Ok(device_id)
    }

    fn read_device_id(&self) -> ApiResult<Option<String>> {
        let path = self.device_path();
        let bytes = match fs::read(&path) {
            Ok(bytes) => bytes,
            Err(err) if err.kind() == io::ErrorKind::NotFound => return Ok(None),
            Err(err) => {
                return Err(ApiError::config(format!(
                    "无法读取设备标识 {}：{err}",
                    path.display()
                )))
            }
        };
        let Ok(file) = serde_json::from_slice::<DeviceFile>(&bytes) else {
            return Ok(None);
        };
        let device_id = file.device_id.trim().to_string();
        Ok(if device_id.is_empty() {
            None
        } else {
            Some(device_id)
        })
    }

    fn save_device_id(&self, device_id: &str) -> ApiResult<()> {
        self.ensure_dir()?;
        let bytes = serde_json::to_vec_pretty(&DeviceFile {
            device_id: device_id.to_string(),
        })
        .map_err(|_| ApiError::config("无法序列化设备标识"))?;
        let path = self.device_path();
        write_private(&path, &bytes).map_err(|err| {
            ApiError::config(format!("无法写入设备标识 {}：{err}", path.display()))
        })
    }

    fn ensure_dir(&self) -> ApiResult<()> {
        fs::create_dir_all(&self.dir).map_err(|err| {
            ApiError::config(format!(
                "无法创建配置目录 {}：{err}",
                self.dir.display()
            ))
        })
    }
}

/// Generates a stable-format (UUID-like) device id without extra dependencies.
///
/// Not a cryptographic UUID: uniqueness comes from the clock, process id and a
/// per-process counter, which is enough for Emby's `DeviceId` semantics.
pub fn generate_device_id() -> String {
    use std::sync::atomic::{AtomicU64, Ordering};
    static COUNTER: AtomicU64 = AtomicU64::new(0);

    let nanos = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|duration| duration.as_nanos() as u64)
        .unwrap_or(0);
    let mut state = nanos
        ^ ((std::process::id() as u64) << 32)
        ^ COUNTER
            .fetch_add(1, Ordering::Relaxed)
            .wrapping_mul(0x9E37_79B9_7F4A_7C15);
    if state == 0 {
        state = 1;
    }
    let mut hex = String::with_capacity(32);
    for _ in 0..32 {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        hex.push(char::from_digit((state & 0xF) as u32, 16).unwrap_or('0'));
    }
    format!(
        "{}-{}-{}-{}-{}",
        &hex[0..8],
        &hex[8..12],
        &hex[12..16],
        &hex[16..20],
        &hex[20..32]
    )
}

fn write_private(path: &Path, bytes: &[u8]) -> io::Result<()> {
    let tmp = path.with_extension("json.tmp");
    let result = (|| -> io::Result<()> {
        let mut options = OpenOptions::new();
        options.write(true).create(true).truncate(true);
        #[cfg(unix)]
        {
            use std::os::unix::fs::OpenOptionsExt;
            options.mode(0o600);
        }
        let mut file = options.open(&tmp)?;
        file.write_all(bytes)?;
        file.sync_all()?;
        drop(file);
        // `fs::rename` replaces an existing target on both Unix and Windows.
        fs::rename(&tmp, path)
    })();
    if result.is_err() {
        let _ = fs::remove_file(&tmp);
    }
    result
}

#[cfg(test)]
mod tests {
    use super::*;

    struct TempDir(PathBuf);

    impl TempDir {
        fn new(tag: &str) -> Self {
            let nanos = SystemTime::now()
                .duration_since(UNIX_EPOCH)
                .unwrap()
                .as_nanos();
            let path = std::env::temp_dir().join(format!(
                "emby-api-{tag}-{}-{nanos}",
                std::process::id()
            ));
            fs::create_dir_all(&path).unwrap();
            Self(path)
        }
    }

    impl Drop for TempDir {
        fn drop(&mut self) {
            let _ = fs::remove_dir_all(&self.0);
        }
    }

    fn session() -> StoredSession {
        StoredSession {
            server_url: "http://192.168.1.10:8096".into(),
            access_token: "token-abc".into(),
            user_id: "user-1".into(),
            user_name: "alice".into(),
            server_name: "Home Emby".into(),
            device_id: "device-1".into(),
        }
    }

    #[test]
    fn saves_and_loads_session_camel_case() {
        let dir = TempDir::new("session-roundtrip");
        let store = SessionStore::new(&dir.0);
        assert_eq!(store.load().unwrap(), None);

        store.save(&session()).unwrap();
        let raw = fs::read_to_string(store.session_path()).unwrap();
        assert!(raw.contains("\"serverUrl\""), "{raw}");
        assert!(raw.contains("\"accessToken\""), "{raw}");
        assert!(!raw.contains("Pw"), "{raw}");
        assert_eq!(store.load().unwrap(), Some(session()));

        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            let mode = fs::metadata(store.session_path())
                .unwrap()
                .permissions()
                .mode()
                & 0o777;
            assert_eq!(mode, 0o600, "session file must be private");
        }
    }

    #[test]
    fn clear_removes_only_the_session() {
        let dir = TempDir::new("session-clear");
        let store = SessionStore::new(&dir.0);
        store.save(&session()).unwrap();
        let device_id = store.device_id().unwrap();

        store.clear().unwrap();
        assert_eq!(store.load().unwrap(), None);
        assert!(!store.session_path().exists());
        // Idempotent.
        store.clear().unwrap();
        // Device id survives logout.
        assert_eq!(store.device_id().unwrap(), device_id);
        assert!(store.device_path().exists());
    }

    #[test]
    fn corrupt_session_is_treated_as_logged_out() {
        let dir = TempDir::new("session-corrupt");
        let store = SessionStore::new(&dir.0);
        fs::write(store.session_path(), b"{not json").unwrap();
        assert_eq!(store.load().unwrap(), None);

        fs::write(
            store.session_path(),
            br#"{"serverUrl":"http://h","accessToken":"","userId":"u"}"#,
        )
        .unwrap();
        assert_eq!(store.load().unwrap(), None);
    }

    #[test]
    fn device_id_is_stable_and_uuid_like() {
        let dir = TempDir::new("device-id");
        let store = SessionStore::new(&dir.0);
        let first = store.device_id().unwrap();
        let second = SessionStore::new(&dir.0).device_id().unwrap();
        assert_eq!(first, second);
        assert_eq!(first.len(), 36, "{first}");
        let groups: Vec<&str> = first.split('-').collect();
        assert_eq!(groups.len(), 5, "{first}");
        assert_eq!(
            groups.iter().map(|group| group.len()).collect::<Vec<_>>(),
            vec![8, 4, 4, 4, 12]
        );
        assert!(groups
            .iter()
            .all(|group| group.chars().all(|c| c.is_ascii_hexdigit())));

        let generated = generate_device_id();
        assert_ne!(first, generated);
        assert_ne!(generated, generate_device_id());
    }

    #[test]
    fn migrates_device_id_from_an_older_session() {
        let dir = TempDir::new("device-migrate");
        let store = SessionStore::new(&dir.0);
        let mut old = session();
        old.device_id = "legacy-device".into();
        store.save(&old).unwrap();
        assert!(!store.device_path().exists());

        assert_eq!(store.device_id().unwrap(), "legacy-device");
        assert!(store.device_path().exists());
        store.clear().unwrap();
        assert_eq!(store.device_id().unwrap(), "legacy-device");
    }

    #[test]
    fn debug_redacts_access_token() {
        let text = format!("{:?}", session());
        assert!(text.contains("server_url"), "{text}");
        assert!(!text.contains("token-abc"), "{text}");
        assert!(text.contains("<redacted>"), "{text}");
    }

    #[test]
    fn public_session_never_contains_the_token() {
        let value = serde_json::to_value(session().public()).unwrap();
        assert_eq!(value["userId"], "user-1");
        assert!(value.get("accessToken").is_none());
        assert!(!value.to_string().contains("token-abc"));
    }
}
