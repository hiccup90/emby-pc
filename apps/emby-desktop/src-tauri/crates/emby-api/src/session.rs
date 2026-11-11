//! Session and device-id persistence.
//!
//! The app hands in the Tauri app config dir; this module owns these files:
//! - `session.json`: server id, server URL/name, user id/name, device id,
//!   access token and a small format version. There is deliberately no
//!   password: this build only persists the token.
//! - `device.json`: the Emby `DeviceId`. It survives logout so the server keeps
//!   associating the client with the same device.
//!
//! On Windows the whole session JSON blob is protected with user-scoped DPAPI
//! (see `secret`); on other targets `secret::protect` is a stub and the JSON is
//! stored as plain text. Writes use a temp file + rename so a crash does not
//! leave a truncated primary file; there is no backup copy and no checksum.
//! A corrupt file is reported as a storage error and deliberately retained.
//! Legacy `session.json.bak` / `session.json.tmp` files from older builds are
//! removed once on the first load (boot).

use std::fmt;
use std::fs::{self, OpenOptions};
use std::io::{self, Write};
use std::path::{Path, PathBuf};
use std::time::{SystemTime, UNIX_EPOCH};

use serde::{Deserialize, Serialize};

use crate::error::{ApiError, ApiResult};
use crate::model::EmbySession;
use crate::secret;

pub const SESSION_FILE: &str = "session.json";
pub const DEVICE_FILE: &str = "device.json";
pub const SESSION_VERSION: u32 = 1;

const ENCRYPTED_MAGIC: &[u8] = b"EMBY-DPAPI-1\0";
const LEGACY_BACKUP_FILE: &str = "session.json.bak";
const LEGACY_TEMP_FILE: &str = "session.json.tmp";

fn default_version() -> u32 {
    SESSION_VERSION
}

/// Persisted login state. `Debug` is implemented manually so the token cannot
/// leak through an accidental `{:?}`.
#[derive(Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct StoredSession {
    /// Stable Emby server id, when the login response supplied one.
    #[serde(default)]
    pub server_id: Option<String>,
    pub server_url: String,
    pub server_name: String,
    pub user_id: String,
    pub user_name: String,
    pub device_id: String,
    pub access_token: String,
    /// File-format version. Missing in files written by older builds.
    #[serde(default = "default_version")]
    pub version: u32,
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
            .field("server_id", &self.server_id)
            .field("server_url", &self.server_url)
            .field("server_name", &self.server_name)
            .field("user_id", &self.user_id)
            .field("user_name", &self.user_name)
            .field("device_id", &self.device_id)
            .field("access_token", &"<redacted>")
            .field("version", &self.version)
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

    /// Removes recovery files written by the previous implementation. This is
    /// intentionally a deletion, not a fallback: they contain an old token and
    /// the new design does not use multiple copies.
    pub fn cleanup_legacy_files(&self) -> ApiResult<()> {
        let mut first_error: Option<ApiError> = None;
        for name in [LEGACY_BACKUP_FILE, LEGACY_TEMP_FILE] {
            let path = self.dir.join(name);
            match fs::remove_file(&path) {
                Ok(()) => {}
                Err(err) if err.kind() == io::ErrorKind::NotFound => {}
                Err(err) => {
                    if first_error.is_none() {
                        first_error = Some(storage_error(format!(
                            "无法删除遗留会话文件 {}：{err}",
                            path.display()
                        )));
                    }
                }
            }
        }
        match first_error {
            Some(err) => Err(err),
            None => Ok(()),
        }
    }

    /// Reads the single stored session. Missing file means signed out
    /// (`Ok(None)`); corrupt or unreadable data is a storage error and the
    /// file is left in place so it can be inspected or overwritten by a later
    /// successful login.
    pub fn load(&self) -> ApiResult<Option<StoredSession>> {
        if let Err(err) = self.cleanup_legacy_files() {
            eprintln!("[emby] could not remove legacy session copies: {err}");
        }

        let path = self.session_path();
        let bytes = match fs::read(&path) {
            Ok(bytes) => bytes,
            Err(err) if err.kind() == io::ErrorKind::NotFound => return Ok(None),
            Err(err) => {
                return Err(storage_error(format!(
                    "无法读取会话文件 {}：{err}",
                    path.display()
                )))
            }
        };

        let json = if bytes.starts_with(ENCRYPTED_MAGIC) {
            secret::unprotect(&bytes[ENCRYPTED_MAGIC.len()..]).map_err(|err| {
                storage_error(format!(
                    "无法解密会话文件 {}：{err}",
                    path.display()
                ))
            })?
        } else {
            bytes
        };

        let session = serde_json::from_slice::<StoredSession>(&json).map_err(|_| {
            storage_error(format!(
                "会话文件损坏，已保留等待重新登录后覆盖：{}",
                path.display()
            ))
        })?;

        if session.version > SESSION_VERSION {
            return Err(storage_error(format!(
                "会话文件版本 {} 高于当前支持的 {}：{}",
                session.version,
                SESSION_VERSION,
                path.display()
            )));
        }
        if session.access_token.trim().is_empty()
            || session.server_url.trim().is_empty()
            || session.user_id.trim().is_empty()
            || session.device_id.trim().is_empty()
        {
            return Err(storage_error(format!(
                "会话文件缺少必要字段，已保留：{}",
                path.display()
            )));
        }

        Ok(Some(session))
    }

    /// Persists the session using one temp file + rename. On Windows the JSON
    /// is DPAPI-encrypted; a protection failure falls back to an unencrypted
    /// file so login still succeeds, and the reason is logged without secrets.
    pub fn save(&self, session: &StoredSession) -> ApiResult<()> {
        self.ensure_dir()?;
        let json = serde_json::to_vec_pretty(session)
            .map_err(|_| ApiError::config("无法序列化会话数据"))?;

        let bytes = match secret::protect(&json) {
            Ok(Some(protected)) => {
                let mut out = Vec::with_capacity(ENCRYPTED_MAGIC.len() + protected.len());
                out.extend_from_slice(ENCRYPTED_MAGIC);
                out.extend_from_slice(&protected);
                out
            }
            Ok(None) => json,
            Err(err) => {
                eprintln!("[emby] session protection failed ({err}); storing unencrypted session");
                json
            }
        };

        let path = self.session_path();
        write_atomic(&path, &bytes).map_err(|err| {
            storage_error(format!("无法写入会话文件 {}：{err}", path.display()))
        })
    }

    /// Removes the session and any legacy recovery copies. Missing files are
    /// not an error; a real removal failure is reported so logout cannot
    /// pretend the token is gone.
    pub fn clear(&self) -> ApiResult<()> {
        let path = self.session_path();
        let mut first_error: Option<ApiError> = match fs::remove_file(&path) {
            Ok(()) => None,
            Err(err) if err.kind() == io::ErrorKind::NotFound => None,
            Err(err) => Some(storage_error(format!(
                "无法删除会话文件 {}：{err}",
                path.display()
            ))),
        };

        if let Err(err) = self.cleanup_legacy_files() {
            if first_error.is_none() {
                first_error = Some(err);
            }
        }

        match first_error {
            Some(err) => Err(err),
            None => Ok(()),
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
        // A corrupt session must not block login: the user can overwrite it
        // after a successful login. If device.json is also missing, a fresh
        // device id is the only recoverable option.
        match self.load() {
            Ok(Some(session)) => {
                let device_id = session.device_id.trim().to_string();
                if !device_id.is_empty() {
                    self.save_device_id(&device_id)?;
                    return Ok(device_id);
                }
            }
            Ok(None) => {}
            Err(err) => {
                eprintln!("[emby] could not read a legacy device id: {err}");
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
        write_atomic(&path, &bytes).map_err(|err| {
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

fn storage_error(message: impl Into<String>) -> ApiError {
    ApiError::config(format!("会话存储错误：{}", message.into()))
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

/// Writes a private file via a sibling temp file and rename. The temp file
/// shares the legacy `session.json.tmp` name for session writes, but it is
/// removed by rename on success and by the error path on failure.
fn write_atomic(path: &Path, bytes: &[u8]) -> io::Result<()> {
    let file_name = path
        .file_name()
        .map(|name| name.to_string_lossy().into_owned())
        .unwrap_or_default();
    let tmp = path.with_file_name(format!("{file_name}.tmp"));
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

    fn test_token() -> String {
        vec!["test", "access", "token"].join("-")
    }

    fn session() -> StoredSession {
        StoredSession {
            server_id: Some("server-1".into()),
            server_url: "http://192.168.1.10:8096".into(),
            server_name: "Home Emby".into(),
            user_id: "user-1".into(),
            user_name: "alice".into(),
            device_id: "device-1".into(),
            access_token: test_token(),
            version: SESSION_VERSION,
        }
    }

    #[test]
    fn saves_and_loads_session() {
        let dir = TempDir::new("session-roundtrip");
        let store = SessionStore::new(&dir.0);
        assert_eq!(store.load().unwrap(), None);

        store.save(&session()).unwrap();
        assert_eq!(store.load().unwrap(), Some(session()));
        assert!(!store.dir().join(LEGACY_BACKUP_FILE).exists());
        assert!(!store.dir().join(LEGACY_TEMP_FILE).exists());

        #[cfg(not(windows))]
        {
            let raw = fs::read_to_string(store.session_path()).unwrap();
            assert!(raw.contains("\"serverUrl\""), "{raw}");
            assert!(raw.contains("\"version\""), "{raw}");
            assert!(!raw.to_ascii_lowercase().contains("password"), "{raw}");
            assert!(!raw.contains("\"pw\""), "{raw}");
        }
        #[cfg(windows)]
        {
            let raw = fs::read(store.session_path()).unwrap();
            assert!(raw.starts_with(ENCRYPTED_MAGIC), "DPAPI file must be tagged");
            let token = session().access_token;
            assert!(
                !raw.windows(token.len()).any(|window| window == token.as_bytes()),
                "token must not be stored in plain text on Windows"
            );
        }

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
    fn legacy_session_without_version_or_server_id_loads() {
        let dir = TempDir::new("session-legacy");
        let store = SessionStore::new(&dir.0);
        let legacy = serde_json::json!({
            "serverUrl": "http://192.168.1.10:8096",
            "serverName": "Home Emby",
            "userId": "user-1",
            "userName": "alice",
            "deviceId": "device-1",
            "accessToken": test_token(),
        });
        fs::write(store.session_path(), serde_json::to_vec(&legacy).unwrap()).unwrap();

        let loaded = store.load().unwrap().unwrap();
        assert_eq!(loaded.server_id, None);
        assert_eq!(loaded.version, SESSION_VERSION);
        assert_eq!(loaded.device_id, "device-1");
    }

    #[test]
    fn corrupt_session_is_a_storage_error_and_is_retained() {
        let dir = TempDir::new("session-corrupt");
        let store = SessionStore::new(&dir.0);
        fs::write(store.session_path(), b"{not json").unwrap();

        assert!(store.load().is_err());
        assert!(store.session_path().exists(), "corrupt file must not be deleted");
        assert_eq!(fs::read(store.session_path()).unwrap(), b"{not json".to_vec());
    }

    #[test]
    fn empty_mandatory_fields_are_a_storage_error() {
        let dir = TempDir::new("session-empty-fields");
        let store = SessionStore::new(&dir.0);
        fs::write(
            store.session_path(),
            br#"{"serverUrl":"http://h","serverName":"H","userId":"u","userName":"U","deviceId":"d","accessToken":""}"#,
        )
        .unwrap();

        assert!(store.load().is_err());
        assert!(store.session_path().exists());
    }

    #[test]
    fn cleanup_removes_legacy_recovery_files() {
        let dir = TempDir::new("session-legacy-cleanup");
        let store = SessionStore::new(&dir.0);
        let backup = store.dir().join(LEGACY_BACKUP_FILE);
        let temp = store.dir().join(LEGACY_TEMP_FILE);
        fs::write(&backup, b"old token").unwrap();
        fs::write(&temp, b"old temp").unwrap();

        store.cleanup_legacy_files().unwrap();
        assert!(!backup.exists());
        assert!(!temp.exists());
        // Idempotent.
        store.cleanup_legacy_files().unwrap();
    }

    #[test]
    fn load_also_cleans_legacy_files() {
        let dir = TempDir::new("session-load-cleanup");
        let store = SessionStore::new(&dir.0);
        let backup = store.dir().join(LEGACY_BACKUP_FILE);
        fs::write(&backup, b"old token").unwrap();

        assert_eq!(store.load().unwrap(), None);
        assert!(!backup.exists());
    }

    #[test]
    fn clear_removes_the_session_and_legacy_files_but_keeps_device_id() {
        let dir = TempDir::new("session-clear");
        let store = SessionStore::new(&dir.0);
        store.save(&session()).unwrap();
        let device_id = store.device_id().unwrap();
        fs::write(store.dir().join(LEGACY_BACKUP_FILE), b"old").unwrap();
        fs::write(store.dir().join(LEGACY_TEMP_FILE), b"old").unwrap();

        store.clear().unwrap();
        assert_eq!(store.load().unwrap(), None);
        assert!(!store.session_path().exists());
        assert!(!store.dir().join(LEGACY_BACKUP_FILE).exists());
        assert!(!store.dir().join(LEGACY_TEMP_FILE).exists());
        // Idempotent.
        store.clear().unwrap();
        // Device id survives logout.
        assert_eq!(store.device_id().unwrap(), device_id);
        assert!(store.device_path().exists());
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
        assert!(!text.contains(&test_token()), "{text}");
        assert!(text.contains("<redacted>"), "{text}");
    }

    #[test]
    fn public_session_never_contains_the_token() {
        let value = serde_json::to_value(session().public()).unwrap();
        assert_eq!(value["userId"], "user-1");
        assert!(value.get("accessToken").is_none());
        assert!(!value.to_string().contains(&test_token()));
    }
}
