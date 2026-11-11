//! Tauri bridge for `emby-api`.
//!
//! Responsibilities:
//! - own the one shared `emby_api::HttpClient`;
//! - run the single startup boot flow (`emby_session_boot`) and remember its
//!   result;
//! - map every failure to a serializable, machine-readable `{reason,message}`;
//! - notify the frontend with a Tauri event when a running session loses its
//!   token;
//! - expose the camelCase command contract consumed by the frontend.
//!
//! All commands are `async fn`: Tauri runs them on its tokio runtime, never on
//! the UI thread. Session storage is a tiny JSON file, so its I/O is done
//! inline. Access tokens never reach frontend success values or messages.

use std::fmt;
use std::time::Duration;

use emby_api::{
    authorization_header, build_url, ApiError, ApiErrorKind, EmbyClient, EmbyImageRequest,
    EmbyItem, EmbyItemsPage, EmbyItemsRequest, EmbyLoginRequest, EmbyPlaybackRequest,
    EmbyPlaybackTarget, EmbyReportRequest, EmbyResumeRequest, EmbySession, EmbyView,
    SessionStore, StoredSession, SESSION_VERSION,
};
use serde::Serialize;
use tauri::async_runtime::Mutex;
use tauri::{AppHandle, Emitter, Manager, State};

/// Message shown when the server no longer accepts the stored token.
const AUTH_EXPIRED: &str = "登录已过期，请重新登录";
const TOKEN_CHECK_TIMEOUT: Duration = Duration::from_secs(5);
const LOGOUT_TIMEOUT: Duration = Duration::from_secs(5);
const BOOT_ATTEMPTS: usize = 3;
#[cfg(not(test))]
const BOOT_RETRY_DELAYS: [Duration; 2] = [Duration::from_secs(3), Duration::from_secs(7)];
#[cfg(test)]
const BOOT_RETRY_DELAYS: [Duration; 2] = [Duration::from_millis(10), Duration::from_millis(20)];

/// Event emitted when a running command receives a 401. The frontend must
/// treat this as a signed-out transition and stop playback.
pub const EMBY_SESSION_EVENT: &str = "emby-session";

const DEFAULT_RESUME_LIMIT: u32 = 16;
const DEFAULT_SIMILAR_LIMIT: u32 = 12;
const DEFAULT_PERSON_LIMIT: u32 = 60;

/// Machine-readable reason shared by boot results and command errors.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum SessionReason {
    #[serde(rename = "none")]
    NoSession,
    TokenRejected,
    Unreachable,
    ServerError,
    StorageError,
    InvalidCredentials,
    InvalidRequest,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize)]
#[serde(rename_all = "camelCase")]
pub enum SessionStatus {
    SignedIn,
    SignedOut,
}

/// Startup result. `lastServer` / `lastUserName` are safe prefill hints and
/// are present on signed-out results whenever a readable session was found.
#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct SessionBoot {
    pub status: SessionStatus,
    pub reason: SessionReason,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub session: Option<EmbySession>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub last_server: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub last_user_name: Option<String>,
}

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct SessionEvent {
    pub status: SessionStatus,
    pub reason: SessionReason,
    pub message: String,
}

/// Serialized as the command error value consumed by the frontend.
#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct EmbyCommandError {
    pub reason: SessionReason,
    pub message: String,
}

impl EmbyCommandError {
    fn new(reason: SessionReason, message: impl Into<String>) -> Self {
        Self {
            reason,
            message: message.into(),
        }
    }
}

impl fmt::Display for EmbyCommandError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.message)
    }
}

impl std::error::Error for EmbyCommandError {}

impl SessionBoot {
    fn signed_in(session: &StoredSession) -> Self {
        Self {
            status: SessionStatus::SignedIn,
            reason: SessionReason::NoSession,
            session: Some(session.public()),
            last_server: Some(session.server_url.clone()),
            last_user_name: Some(session.user_name.clone()),
        }
    }

    fn signed_out(reason: SessionReason, session: Option<&StoredSession>) -> Self {
        Self {
            status: SessionStatus::SignedOut,
            reason,
            session: None,
            last_server: session.map(|session| session.server_url.clone()),
            last_user_name: session.map(|session| session.user_name.clone()),
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum TokenProbe {
    Valid,
    TokenRejected,
    Transient(SessionReason),
}

/// Managed state for every `emby_*` command.
pub struct EmbyService {
    http: emby_api::HttpClient,
    store: SessionStore,
    device_name: String,
    app: AppHandle,
    session: Mutex<Option<StoredSession>>,
    boot: Mutex<Option<SessionBoot>>,
}

impl EmbyService {
    pub fn new(app: &AppHandle) -> Result<Self, String> {
        let dir = app
            .path()
            .app_config_dir()
            .map_err(|err| format!("无法定位应用配置目录：{err}"))?;
        let http = emby_api::default_http_client().map_err(|err| err.to_string())?;
        Ok(Self {
            http,
            store: SessionStore::new(dir),
            device_name: emby_api::default_device_name(),
            app: app.clone(),
            session: Mutex::new(None),
            boot: Mutex::new(None),
        })
    }

    /// Single-flight boot. A successful result is cached; a signed-out result
    /// is also cached for the initial mount, but `force = true` allows the
    /// login page's retry action to run the flow again.
    async fn boot(&self, force: bool) -> SessionBoot {
        let mut slot = self.boot.lock().await;
        if !force {
            if let Some(existing) = slot.as_ref() {
                return existing.clone();
            }
        }
        let result = self.boot_once().await;
        *slot = Some(result.clone());
        result
    }

    async fn boot_once(&self) -> SessionBoot {
        let (result, session) = boot_session(&self.store, &self.http, &self.device_name).await;
        self.set_session(session).await;
        result
    }

    async fn set_session(&self, session: Option<StoredSession>) {
        *self.session.lock().await = session;
    }

    async fn current_session(&self) -> Option<StoredSession> {
        self.session.lock().await.clone()
    }

    async fn remember(&self, session: StoredSession) {
        self.set_session(Some(session.clone())).await;
        self.set_boot(SessionBoot::signed_in(&session)).await;
    }

    async fn set_boot(&self, result: SessionBoot) {
        *self.boot.lock().await = Some(result);
    }

    async fn forget_local(&self) {
        self.set_session(None).await;
        if let Err(err) = self.store.clear() {
            eprintln!("[emby] could not clear the local session: {err}");
        }
    }

    async fn require_session(&self) -> Result<StoredSession, EmbyCommandError> {
        self.current_session().await.ok_or_else(|| {
            EmbyCommandError::new(SessionReason::NoSession, "尚未登录 Emby 服务器")
        })
    }

    fn client(&self, session: &StoredSession) -> EmbyClient {
        EmbyClient::with_token(
            self.http.clone(),
            session.server_url.clone(),
            session.device_id.clone(),
            self.device_name.clone(),
            session.access_token.clone(),
        )
    }

    /// Maps a non-auth API error to the shared structured error.
    fn command_error(&self, err: ApiError) -> EmbyCommandError {
        let reason = match err.kind() {
            ApiErrorKind::Transport => SessionReason::Unreachable,
            ApiErrorKind::Config => SessionReason::StorageError,
            ApiErrorKind::Unauthorized => SessionReason::TokenRejected,
            ApiErrorKind::Server | ApiErrorKind::Decode | ApiErrorKind::NotFound => {
                SessionReason::ServerError
            }
            ApiErrorKind::NoPlayableSource | ApiErrorKind::Unsupported => {
                SessionReason::ServerError
            }
        };
        EmbyCommandError::new(reason, err.to_string())
    }

    fn login_error(err: ApiError) -> EmbyCommandError {
        let reason = match err.kind() {
            ApiErrorKind::Unauthorized => SessionReason::InvalidCredentials,
            ApiErrorKind::Transport => SessionReason::Unreachable,
            ApiErrorKind::Config => SessionReason::InvalidRequest,
            ApiErrorKind::Server | ApiErrorKind::Decode | ApiErrorKind::NotFound => {
                SessionReason::ServerError
            }
            ApiErrorKind::NoPlayableSource | ApiErrorKind::Unsupported => {
                SessionReason::ServerError
            }
        };
        EmbyCommandError::new(reason, err.to_string())
    }

    /// A 403 is a permission problem, not an expired token. A 401 is handled
    /// once: the first command that observes it clears the session and emits
    /// the signed-out event; concurrent commands see no session and do not
    /// emit again.
    async fn fail(&self, err: ApiError) -> EmbyCommandError {
        if err.status() == Some(403) {
            return EmbyCommandError::new(SessionReason::ServerError, "没有权限执行该操作");
        }
        if err.status() == Some(401) || err.kind() == ApiErrorKind::Unauthorized {
            return self.handle_token_rejected().await;
        }
        self.command_error(err)
    }

    async fn handle_token_rejected(&self) -> EmbyCommandError {
        let session = self.session.lock().await.take();
        if let Some(session) = session {
            if let Err(err) = self.store.clear() {
                eprintln!("[emby] could not clear a rejected session: {err}");
            }
            let result = SessionBoot::signed_out(SessionReason::TokenRejected, Some(&session));
            self.set_boot(result).await;
            let event = SessionEvent {
                status: SessionStatus::SignedOut,
                reason: SessionReason::TokenRejected,
                message: AUTH_EXPIRED.to_string(),
            };
            if let Err(err) = self.app.emit(EMBY_SESSION_EVENT, event) {
                eprintln!("[emby] could not emit the session event: {err}");
            }
        }
        EmbyCommandError::new(SessionReason::TokenRejected, AUTH_EXPIRED)
    }

    async fn logout(&self) -> SessionBoot {
        let previous = self.current_session().await;
        if let Some(session) = previous.as_ref() {
            logout_request(&self.http, &self.device_name, session).await;
        }
        self.forget_local().await;
        let result = SessionBoot::signed_out(SessionReason::NoSession, previous.as_ref());
        self.set_boot(result.clone()).await;
        result
    }
}

/// Pure(ish) boot pass, kept separate from the Tauri handle so it can be
/// exercised with the loopback mock server in tests.
async fn boot_session(
    store: &SessionStore,
    http: &emby_api::HttpClient,
    device_name: &str,
) -> (SessionBoot, Option<StoredSession>) {
    let session = match store.load() {
        Ok(session) => session,
        Err(err) => {
            eprintln!("[emby] session boot storage error: {err}");
            return (SessionBoot::signed_out(SessionReason::StorageError, None), None);
        }
    };
    let Some(session) = session else {
        return (SessionBoot::signed_out(SessionReason::NoSession, None), None);
    };

    let mut final_reason = SessionReason::Unreachable;
    for attempt in 0..BOOT_ATTEMPTS {
        match probe_token(http, device_name, &session).await {
            TokenProbe::Valid => return (SessionBoot::signed_in(&session), Some(session)),
            TokenProbe::TokenRejected => {
                if let Err(err) = store.clear() {
                    eprintln!("[emby] could not clear a rejected session: {err}");
                }
                return (
                    SessionBoot::signed_out(SessionReason::TokenRejected, Some(&session)),
                    None,
                );
            }
            TokenProbe::Transient(reason) => {
                final_reason = reason;
                if attempt + 1 < BOOT_ATTEMPTS {
                    std::thread::sleep(BOOT_RETRY_DELAYS[attempt]);
                }
            }
        }
    }

    (SessionBoot::signed_out(final_reason, Some(&session)), None)
}

async fn probe_token(
    http: &emby_api::HttpClient,
    device_name: &str,
    session: &StoredSession,
) -> TokenProbe {
    let Ok(url) = build_url(&session.server_url, &["Users", session.user_id.as_str()], &[]) else {
        return TokenProbe::Transient(SessionReason::ServerError);
    };
    let authorization = authorization_header(
        device_name,
        &session.device_id,
        Some(session.access_token.as_str()),
    );
    let response = http
        .get(url.as_str())
        .timeout(TOKEN_CHECK_TIMEOUT)
        .header("X-Emby-Authorization", authorization.as_str())
        .header("X-Emby-Token", session.access_token.as_str())
        .header("Accept", "application/json")
        .send()
        .await;
    let Ok(response) = response else {
        return TokenProbe::Transient(SessionReason::Unreachable);
    };
    match response.status().as_u16() {
        200..=299 => TokenProbe::Valid,
        401 => TokenProbe::TokenRejected,
        403 => TokenProbe::Valid,
        500..=599 => TokenProbe::Transient(SessionReason::ServerError),
        _ => TokenProbe::Transient(SessionReason::ServerError),
    }
}

async fn logout_request(
    http: &emby_api::HttpClient,
    device_name: &str,
    session: &StoredSession,
) {
    let Ok(url) = build_url(&session.server_url, &["Sessions", "Logout"], &[]) else {
        return;
    };
    let authorization = authorization_header(
        device_name,
        &session.device_id,
        Some(session.access_token.as_str()),
    );
    let _ = http
        .post(url.as_str())
        .timeout(LOGOUT_TIMEOUT)
        .header("X-Emby-Authorization", authorization.as_str())
        .header("X-Emby-Token", session.access_token.as_str())
        .send()
        .await;
}

#[tauri::command]
pub async fn emby_login(
    req: EmbyLoginRequest,
    state: State<'_, EmbyService>,
) -> Result<EmbySession, EmbyCommandError> {
    let server_url = emby_api::normalize_server_url(&req.server)
        .map_err(|err| EmbyCommandError::new(SessionReason::InvalidRequest, err.to_string()))?;
    if req.username.trim().is_empty() {
        return Err(EmbyCommandError::new(
            SessionReason::InvalidRequest,
            "请输入用户名",
        ));
    }

    let device_id = state
        .store
        .device_id()
        .map_err(|err| EmbyCommandError::new(SessionReason::StorageError, err.to_string()))?;
    let client = EmbyClient::new(
        state.http.clone(),
        server_url.clone(),
        device_id.clone(),
        state.device_name.clone(),
    );
    let outcome = client
        .login(&req.username, &req.password)
        .await
        .map_err(EmbyService::login_error)?;

    let session = StoredSession {
        server_id: outcome.server_id,
        server_url,
        server_name: outcome.server_name,
        user_id: outcome.user_id,
        user_name: outcome.user_name,
        device_id,
        access_token: outcome.access_token,
        version: SESSION_VERSION,
    };
    // The in-memory session is usable even if the disk write fails; the error
    // tells the frontend that it is not persistent.
    state.remember(session.clone()).await;
    if let Err(err) = state.store.save(&session) {
        return Err(EmbyCommandError::new(
            SessionReason::StorageError,
            format!("登录成功，但无法保存会话：{err}"),
        ));
    }
    Ok(session.public())
}

#[tauri::command]
pub async fn emby_session_boot(
    force: Option<bool>,
    state: State<'_, EmbyService>,
) -> Result<SessionBoot, EmbyCommandError> {
    Ok(state.boot(force.unwrap_or(false)).await)
}

/// Compatibility command for the pre-state-machine frontend; task-21 replaces
/// it with `emby_session_boot`.
#[tauri::command]
pub async fn emby_session(state: State<'_, EmbyService>) -> Result<Option<EmbySession>, EmbyCommandError> {
    Ok(state.boot(false).await.session)
}

#[tauri::command]
pub async fn emby_logout(state: State<'_, EmbyService>) -> Result<SessionBoot, EmbyCommandError> {
    Ok(state.logout().await)
}

#[tauri::command]
pub async fn emby_views(state: State<'_, EmbyService>) -> Result<Vec<EmbyView>, EmbyCommandError> {
    let session = state.require_session().await?;
    let client = state.client(&session);
    match client.views(&session.user_id).await {
        Ok(views) => Ok(views),
        Err(err) => Err(state.fail(err).await),
    }
}

#[tauri::command]
pub async fn emby_items(
    req: EmbyItemsRequest,
    state: State<'_, EmbyService>,
) -> Result<EmbyItemsPage, EmbyCommandError> {
    let session = state.require_session().await?;
    let client = state.client(&session);
    match client.items(&session.user_id, &req).await {
        Ok(page) => Ok(page),
        Err(err) => Err(state.fail(err).await),
    }
}

#[tauri::command]
pub async fn emby_resume(
    req: EmbyResumeRequest,
    state: State<'_, EmbyService>,
) -> Result<EmbyItemsPage, EmbyCommandError> {
    let session = state.require_session().await?;
    let client = state.client(&session);
    let limit = req.limit.unwrap_or(DEFAULT_RESUME_LIMIT);
    match client.resume(&session.user_id, limit).await {
        Ok(page) => Ok(page),
        Err(err) => Err(state.fail(err).await),
    }
}

#[tauri::command]
pub async fn emby_item(
    id: String,
    state: State<'_, EmbyService>,
) -> Result<EmbyItem, EmbyCommandError> {
    let session = state.require_session().await?;
    let client = state.client(&session);
    match client.item(&session.user_id, &id).await {
        Ok(item) => Ok(item),
        Err(err) => Err(state.fail(err).await),
    }
}

#[tauri::command]
pub async fn emby_similar(
    id: String,
    limit: Option<u32>,
    state: State<'_, EmbyService>,
) -> Result<EmbyItemsPage, EmbyCommandError> {
    let session = state.require_session().await?;
    let client = state.client(&session);
    let limit = limit.unwrap_or(DEFAULT_SIMILAR_LIMIT).clamp(1, 50);
    match client.similar(&session.user_id, &id, limit).await {
        Ok(page) => Ok(page),
        Err(err) => Err(state.fail(err).await),
    }
}

#[tauri::command]
pub async fn emby_person_items(
    person_id: String,
    limit: Option<u32>,
    state: State<'_, EmbyService>,
) -> Result<EmbyItemsPage, EmbyCommandError> {
    let session = state.require_session().await?;
    let client = state.client(&session);
    let limit = limit.unwrap_or(DEFAULT_PERSON_LIMIT).clamp(1, 200);
    match client.person_items(&session.user_id, &person_id, limit).await {
        Ok(page) => Ok(page),
        Err(err) => Err(state.fail(err).await),
    }
}

#[tauri::command]
pub async fn emby_set_played(
    id: String,
    played: bool,
    state: State<'_, EmbyService>,
) -> Result<(), EmbyCommandError> {
    let session = state.require_session().await?;
    let client = state.client(&session);
    match client.set_played(&session.user_id, &id, played).await {
        Ok(()) => Ok(()),
        Err(err) => Err(state.fail(err).await),
    }
}

#[tauri::command]
pub async fn emby_set_favorite(
    id: String,
    favorite: bool,
    state: State<'_, EmbyService>,
) -> Result<(), EmbyCommandError> {
    let session = state.require_session().await?;
    let client = state.client(&session);
    match client.set_favorite(&session.user_id, &id, favorite).await {
        Ok(()) => Ok(()),
        Err(err) => Err(state.fail(err).await),
    }
}

#[tauri::command]
pub async fn emby_playback_target(
    req: EmbyPlaybackRequest,
    state: State<'_, EmbyService>,
) -> Result<EmbyPlaybackTarget, EmbyCommandError> {
    let session = state.require_session().await?;
    let client = state.client(&session);
    match client.playback_target(&session.user_id, &req).await {
        Ok(target) => Ok(target),
        Err(err) => Err(state.fail(err).await),
    }
}

#[tauri::command]
pub async fn emby_report_playback(
    req: EmbyReportRequest,
    state: State<'_, EmbyService>,
) -> Result<(), EmbyCommandError> {
    let session = state.require_session().await?;
    let client = state.client(&session);
    match client.report_playback(&req).await {
        Ok(()) => Ok(()),
        Err(err) => Err(state.fail(err).await),
    }
}

#[tauri::command]
pub async fn emby_image_url(
    req: EmbyImageRequest,
    state: State<'_, EmbyService>,
) -> Result<String, EmbyCommandError> {
    let session = state.require_session().await?;
    let client = state.client(&session);
    client.image_url(&req).map_err(|err| state.command_error(err))
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::{Read, Write};
    use std::net::TcpListener;

    fn test_session(server_url: &str) -> StoredSession {
        StoredSession {
            server_id: Some("server-1".into()),
            server_url: server_url.into(),
            server_name: "Test Emby".into(),
            user_id: "user-1".into(),
            user_name: "alice".into(),
            device_id: "device-1".into(),
            access_token: vec!["test", "access", "token"].join("-"),
            version: SESSION_VERSION,
        }
    }

    fn spawn_status_server(status: &'static str) -> (String, std::thread::JoinHandle<String>) {
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let addr = listener.local_addr().unwrap();
        let handle = std::thread::spawn(move || {
            let (mut stream, _) = listener.accept().unwrap();
            let mut request = [0_u8; 4096];
            let size = stream.read(&mut request).unwrap_or(0);
            let request = String::from_utf8_lossy(&request[..size]).to_string();
            let response = format!(
                "HTTP/1.1 {status}\r\nContent-Type: application/json\r\nContent-Length: 2\r\nConnection: close\r\n\r\n{{}}"
            );
            stream.write_all(response.as_bytes()).unwrap();
            request
        });
        (format!("http://{addr}"), handle)
    }

    fn spawn_sequence_server(statuses: &'static [&'static str]) -> (String, std::thread::JoinHandle<Vec<String>>) {
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let addr = listener.local_addr().unwrap();
        let handle = std::thread::spawn(move || {
            let mut requests = Vec::new();
            for &status in statuses {
                let (mut stream, _) = listener.accept().unwrap();
                let mut request = [0_u8; 4096];
                let size = stream.read(&mut request).unwrap_or(0);
                requests.push(String::from_utf8_lossy(&request[..size]).to_string());
                let response = format!(
                    "HTTP/1.1 {status}\r\nContent-Type: application/json\r\nContent-Length: 2\r\nConnection: close\r\n\r\n{{}}"
                );
                stream.write_all(response.as_bytes()).unwrap();
            }
            requests
        });
        (format!("http://{addr}"), handle)
    }

    fn temp_store(tag: &str) -> (SessionStore, std::path::PathBuf) {
        let nanos = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap()
            .as_nanos();
        let dir = std::env::temp_dir().join(format!("emby-desktop-{tag}-{}-{nanos}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        (SessionStore::new(&dir), dir)
    }

    #[test]
    fn probe_401_is_token_rejected() {
        let (url, server) = spawn_status_server("401 Unauthorized");
        let http = emby_api::default_http_client().unwrap();
        let session = test_session(&url);
        let result = tauri::async_runtime::block_on(probe_token(&http, "test", &session));
        assert_eq!(result, TokenProbe::TokenRejected);
        let _ = server.join();
    }

    #[test]
    fn probe_unreachable_is_transient_and_does_not_require_a_server() {
        let http = emby_api::default_http_client().unwrap();
        let session = test_session("http://127.0.0.1:1");
        let result = tauri::async_runtime::block_on(probe_token(&http, "test", &session));
        assert_eq!(result, TokenProbe::Transient(SessionReason::Unreachable));
    }

    #[test]
    fn boot_401_clears_the_persisted_session() {
        let (url, server) = spawn_status_server("401 Unauthorized");
        let (store, dir) = temp_store("boot-401");
        let session = test_session(&url);
        store.save(&session).unwrap();
        let http = emby_api::default_http_client().unwrap();
        let (boot, memory) = tauri::async_runtime::block_on(boot_session(&store, &http, "test"));
        assert_eq!(boot.status, SessionStatus::SignedOut);
        assert_eq!(boot.reason, SessionReason::TokenRejected);
        assert!(memory.is_none());
        assert!(!store.session_path().exists());
        let _ = server.join();
        let _ = std::fs::remove_dir_all(dir);
    }

    #[test]
    fn boot_retries_transient_failures_before_signing_in() {
        let (url, server) = spawn_sequence_server(&[
            "500 Internal Server Error",
            "500 Internal Server Error",
            "200 OK",
        ]);
        let (store, dir) = temp_store("boot-retry");
        let session = test_session(&url);
        store.save(&session).unwrap();
        let http = emby_api::default_http_client().unwrap();
        let (boot, memory) = tauri::async_runtime::block_on(boot_session(&store, &http, "test"));
        assert_eq!(boot.status, SessionStatus::SignedIn);
        assert!(memory.is_some());
        let requests = server.join().unwrap();
        assert_eq!(requests.len(), 3, "boot should probe three times");
        let _ = std::fs::remove_dir_all(dir);
    }

    #[test]
    fn boot_unreachable_keeps_the_persisted_session() {
        let (store, dir) = temp_store("boot-unreachable");
        let session = test_session("http://127.0.0.1:1");
        store.save(&session).unwrap();
        let http = emby_api::default_http_client().unwrap();
        let (boot, memory) = tauri::async_runtime::block_on(boot_session(&store, &http, "test"));
        assert_eq!(boot.status, SessionStatus::SignedOut);
        assert_eq!(boot.reason, SessionReason::Unreachable);
        assert!(memory.is_none());
        assert!(store.session_path().exists(), "sign-in credentials must be retained");
        let _ = std::fs::remove_dir_all(dir);
    }

    #[test]
    fn logout_request_uses_the_logout_endpoint() {
        let (url, server) = spawn_status_server("200 OK");
        let http = emby_api::default_http_client().unwrap();
        let session = test_session(&url);
        tauri::async_runtime::block_on(logout_request(&http, "test", &session));
        let request = server.join().unwrap();
        assert!(request.starts_with("POST /Sessions/Logout "), "{request}");
    }
}
