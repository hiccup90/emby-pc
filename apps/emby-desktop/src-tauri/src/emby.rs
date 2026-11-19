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
//! inline. Tokens are never logged or returned as standalone fields; playback
//! and image URLs do carry `api_key`, because Emby requires it.

use std::fmt;
use std::time::Duration;

use emby_api::{
    ApiError, ApiErrorKind, EmbyApplyRemoteSearchRequest, EmbyClient, EmbyImageRequest, EmbyItem,
    EmbyItemsPage, EmbyItemsRequest, EmbyLoginRequest, EmbyPlaybackRequest, EmbyPlaybackTarget,
    EmbyRemoteSearchRequest, EmbyReportRequest, EmbyResumeRequest, EmbySession, EmbyView,
    SessionStore, StoredSession, SESSION_VERSION,
};
use serde::Serialize;
use tauri::async_runtime::Mutex;
use tauri::{AppHandle, Emitter, Manager, State};

/// Message shown when the server no longer accepts the stored token.
const AUTH_EXPIRED: &str = "登录已过期，请重新登录";
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

/// Cached boot outcome. The active session itself lives only in
/// `EmbyService::session`; this type keeps the signed-out result and its safe
/// prefill hints, so a token rejection cannot leave a second, stale session
/// object behind.
#[derive(Clone, Debug)]
struct BootState {
    status: SessionStatus,
    reason: SessionReason,
    last_server: Option<String>,
    last_user_name: Option<String>,
}

impl BootState {
    fn signed_in(session: &StoredSession) -> Self {
        Self {
            status: SessionStatus::SignedIn,
            reason: SessionReason::NoSession,
            last_server: Some(session.server_url.clone()),
            last_user_name: Some(session.user_name.clone()),
        }
    }

    fn signed_out(reason: SessionReason, session: Option<&StoredSession>) -> Self {
        Self {
            status: SessionStatus::SignedOut,
            reason,
            last_server: session.map(|session| session.server_url.clone()),
            last_user_name: session.map(|session| session.user_name.clone()),
        }
    }

    /// Combine the cached status with the one active session. `boot.session`
    /// is therefore derived on every read instead of being copied at boot.
    fn view(&self, session: Option<&StoredSession>) -> SessionBoot {
        SessionBoot {
            status: self.status,
            reason: self.reason,
            session: if self.status == SessionStatus::SignedIn {
                session.map(StoredSession::public)
            } else {
                None
            },
            last_server: self.last_server.clone(),
            last_user_name: self.last_user_name.clone(),
        }
    }
}

/// Result of one token probe during boot, derived only from
/// `ApiErrorKind` — the app never inspects an HTTP status.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum ProbeOutcome {
    Valid,
    TokenRejected,
    /// The user record is missing (404). This can be a deleted user, a changed
    /// reverse-proxy path, or a different server behind the same URL. Keep the
    /// stored session: it is the only copy of the token.
    NotFound,
    /// Network trouble: worth retrying, and the stored session must be kept so
    /// a later launch can sign in without the password.
    Retryable(SessionReason),
    /// A deterministic server/protocol failure. Retrying only delays the
    /// signed-out screen by the full backoff.
    Fatal(SessionReason),
}

/// Managed state for every `emby_*` command.
pub struct EmbyService {
    http: emby_api::HttpClient,
    store: SessionStore,
    device_name: String,
    app: AppHandle,
    session: Mutex<Option<StoredSession>>,
    boot: Mutex<Option<BootState>>,
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
                let session = self.current_session().await;
                return existing.view(session.as_ref());
            }
        }
        let state = self.boot_once().await;
        *slot = Some(state.clone());
        let session = self.current_session().await;
        state.view(session.as_ref())
    }

    async fn boot_once(&self) -> BootState {
        let (state, session) = boot_session(&self.store, &self.http, &self.device_name).await;
        self.set_session(session).await;
        state
    }

    async fn set_session(&self, session: Option<StoredSession>) {
        *self.session.lock().await = session;
    }

    async fn current_session(&self) -> Option<StoredSession> {
        self.session.lock().await.clone()
    }

    async fn remember(&self, session: StoredSession) {
        self.set_session(Some(session.clone())).await;
        self.set_boot(BootState::signed_in(&session)).await;
    }

    async fn set_boot(&self, state: BootState) {
        *self.boot.lock().await = Some(state);
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

    /// The only error mapping in the app layer: an [`ApiErrorKind`] is already
    /// semantic, so no HTTP status or message text is inspected here.
    ///
    /// `Forbidden` (403) is a permission problem, never an expired session;
    /// only `TokenExpired` (401) clears the session, and it is handled once in
    /// [`Self::fail`] so concurrent commands cannot emit twice.
    fn command_error(err: ApiError) -> EmbyCommandError {
        let reason = match err.kind() {
            ApiErrorKind::TokenExpired => SessionReason::TokenRejected,
            ApiErrorKind::InvalidCredentials => SessionReason::InvalidCredentials,
            ApiErrorKind::InvalidInput => SessionReason::InvalidRequest,
            ApiErrorKind::Storage => SessionReason::StorageError,
            ApiErrorKind::Network => SessionReason::Unreachable,
            ApiErrorKind::Forbidden
            | ApiErrorKind::NotFound
            | ApiErrorKind::Server
            | ApiErrorKind::Decode
            | ApiErrorKind::NoPlayableSource => SessionReason::ServerError,
        };
        EmbyCommandError::new(reason, err.to_string())
    }

    /// Drops the in-memory session and emits the signed-out event on the first
    /// 401; concurrent commands see no session and do not emit again.
    async fn fail(&self, err: ApiError) -> EmbyCommandError {
        if err.kind() == ApiErrorKind::TokenExpired {
            return self.handle_token_rejected().await;
        }
        Self::command_error(err)
    }

    async fn handle_token_rejected(&self) -> EmbyCommandError {
        let session = self.session.lock().await.take();
        if let Some(session) = session {
            if let Err(err) = self.store.clear() {
                eprintln!("[emby] could not clear a rejected session: {err}");
            }
            let state = BootState::signed_out(SessionReason::TokenRejected, Some(&session));
            self.set_boot(state).await;
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
            // Best effort: an unreachable server must not keep the local
            // session (or the UI) signed in.
            if let Err(err) = self.client(session).logout().await {
                eprintln!("[emby] logout request failed: {err}");
            }
        }
        self.forget_local().await;
        let state = BootState::signed_out(SessionReason::NoSession, previous.as_ref());
        self.set_boot(state.clone()).await;
        state.view(None)
    }
}

/// Pure(ish) boot pass, kept separate from the Tauri handle so it can be
/// exercised with the loopback mock server in tests.
async fn boot_session(
    store: &SessionStore,
    http: &emby_api::HttpClient,
    device_name: &str,
) -> (BootState, Option<StoredSession>) {
    // One-shot migration from the previous storage layout: reading the session
    // is side-effect free, so deleting the legacy copies is an explicit
    // boot-time action.
    if let Err(err) = store.migrate_legacy_files() {
        eprintln!("[emby] could not remove legacy session copies: {err}");
    }
    let session = match store.load() {
        Ok(session) => session,
        Err(err) => {
            eprintln!("[emby] session boot storage error: {err}");
            return (BootState::signed_out(SessionReason::StorageError, None), None);
        }
    };
    let Some(session) = session else {
        return (BootState::signed_out(SessionReason::NoSession, None), None);
    };

    let client = EmbyClient::with_token(
        http.clone(),
        session.server_url.clone(),
        session.device_id.clone(),
        device_name.to_string(),
        session.access_token.clone(),
    );
    let mut final_reason = SessionReason::Unreachable;
    // The delay table is the single definition of both the count and the
    // backoff; adding/removing a delay can no longer desynchronize an index.
    let attempts = BOOT_RETRY_DELAYS.len() + 1;
    for attempt in 0..attempts {
        match probe_token(&client, &session).await {
            ProbeOutcome::Valid => return (BootState::signed_in(&session), Some(session)),
            ProbeOutcome::TokenRejected => {
                if let Err(err) = store.clear() {
                    eprintln!("[emby] could not clear a rejected session: {err}");
                }
                return (
                    BootState::signed_out(SessionReason::TokenRejected, Some(&session)),
                    None,
                );
            }
            ProbeOutcome::NotFound => {
                // 404 is not proof that the token is invalid. Do not delete the
                // only stored credential because a proxy path or server changed.
                return (
                    BootState::signed_out(SessionReason::ServerError, Some(&session)),
                    None,
                );
            }
            ProbeOutcome::Retryable(reason) => {
                final_reason = reason;
                if let Some(delay) = BOOT_RETRY_DELAYS.get(attempt) {
                    // Async sleep: boot is an async command, so a blocking
                    // sleep would freeze a Tauri runtime worker for the whole
                    // backoff.
                    tokio::time::sleep(*delay).await;
                }
            }
            ProbeOutcome::Fatal(reason) => {
                return (BootState::signed_out(reason, Some(&session)), None);
            }
        }
    }

    (BootState::signed_out(final_reason, Some(&session)), None)
}

/// Validates the stored token through the shared client.
///
/// Every outcome comes from [`ApiErrorKind`]; the app never inspects an HTTP
/// status. `Forbidden` means the token works but this user may not read the
/// user record, so it still counts as valid. `NotFound` is kept separate from
/// `TokenExpired`: a 404 can also come from a proxy/path change or a different
/// server, so it must not delete the stored session.
async fn probe_token(client: &EmbyClient, session: &StoredSession) -> ProbeOutcome {
    match client.current_user(&session.user_id).await {
        Ok(()) => ProbeOutcome::Valid,
        Err(err) => match err.kind() {
            ApiErrorKind::TokenExpired => ProbeOutcome::TokenRejected,
            ApiErrorKind::NotFound => ProbeOutcome::NotFound,
            ApiErrorKind::Forbidden => ProbeOutcome::Valid,
            ApiErrorKind::Network => ProbeOutcome::Retryable(SessionReason::Unreachable),
            ApiErrorKind::InvalidCredentials
            | ApiErrorKind::InvalidInput
            | ApiErrorKind::Storage
            | ApiErrorKind::Server
            | ApiErrorKind::Decode
            | ApiErrorKind::NoPlayableSource => ProbeOutcome::Fatal(SessionReason::ServerError),
        },
    }
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
        .map_err(EmbyService::command_error)?;

    let session = StoredSession {
        server_url,
        server_name: outcome.server_name,
        user_id: outcome.user_id,
        user_name: outcome.user_name,
        device_id,
        access_token: outcome.access_token,
        version: SESSION_VERSION,
    };
    // Do not keep a session the next launch cannot read: the login page would
    // show an error while every command ran signed in, then a restart would
    // silently lose it. The disk write is part of the login contract.
    state.remember(session.clone()).await;
    if let Err(err) = state.store.save(&session) {
        state.set_session(None).await;
        state
            .set_boot(BootState::signed_out(SessionReason::StorageError, Some(&session)))
            .await;
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
pub async fn emby_delete_info(
    id: String,
    state: State<'_, EmbyService>,
) -> Result<Vec<String>, EmbyCommandError> {
    let session = state.require_session().await?;
    let client = state.client(&session);
    match client.delete_info(&id).await {
        Ok(paths) => Ok(paths),
        Err(err) => Err(state.fail(err).await),
    }
}

#[tauri::command]
pub async fn emby_delete_items(
    ids: Vec<String>,
    state: State<'_, EmbyService>,
) -> Result<(), EmbyCommandError> {
    let session = state.require_session().await?;
    let client = state.client(&session);
    match client.delete_items(&ids).await {
        Ok(()) => Ok(()),
        Err(err) => Err(state.fail(err).await),
    }
}

#[tauri::command]
pub async fn emby_remote_search(
    req: EmbyRemoteSearchRequest,
    state: State<'_, EmbyService>,
) -> Result<Vec<serde_json::Value>, EmbyCommandError> {
    let session = state.require_session().await?;
    let client = state.client(&session);
    match client.remote_search(&req).await {
        Ok(results) => Ok(results),
        Err(err) => Err(state.fail(err).await),
    }
}

#[tauri::command]
pub async fn emby_apply_remote_search(
    req: EmbyApplyRemoteSearchRequest,
    state: State<'_, EmbyService>,
) -> Result<(), EmbyCommandError> {
    let session = state.require_session().await?;
    let client = state.client(&session);
    match client.apply_remote_search(&req).await {
        Ok(()) => Ok(()),
        Err(err) => Err(state.fail(err).await),
    }
}

/// Builds the server-side proxy URL for a remote-search result image. This is
/// only string assembly; the webview loads the image like any other Emby URL.
#[tauri::command]
pub async fn emby_remote_search_image(
    image_url: String,
    provider: Option<String>,
    state: State<'_, EmbyService>,
) -> Result<String, EmbyCommandError> {
    let session = state.require_session().await?;
    let client = state.client(&session);
    match client.remote_search_image_url(&image_url, provider.as_deref()) {
        Ok(url) => Ok(url),
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
    client.image_url(&req).map_err(EmbyService::command_error)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::{Read, Write};
    use std::net::TcpListener;

    fn test_session(server_url: &str) -> StoredSession {
        StoredSession {
            server_url: server_url.into(),
            server_name: "Test Emby".into(),
            user_id: "user-1".into(),
            user_name: "alice".into(),
            device_id: "device-1".into(),
            access_token: vec!["test", "access", "token"].join("-"),
            version: SESSION_VERSION,
        }
    }

    fn test_client(server_url: &str) -> EmbyClient {
        let session = test_session(server_url);
        EmbyClient::with_token(
            emby_api::default_http_client().unwrap(),
            session.server_url,
            session.device_id,
            "test".to_string(),
            session.access_token,
        )
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
        let client = test_client(&url);
        let session = test_session(&url);
        let result = tauri::async_runtime::block_on(probe_token(&client, &session));
        assert_eq!(result, ProbeOutcome::TokenRejected);
        let _ = server.join();
    }

    #[test]
    fn probe_403_is_not_an_expired_token() {
        // A permission problem must keep the user signed in; treating 403 as
        // "session expired" used to drop a perfectly valid login.
        let (url, server) = spawn_status_server("403 Forbidden");
        let client = test_client(&url);
        let session = test_session(&url);
        let result = tauri::async_runtime::block_on(probe_token(&client, &session));
        assert_eq!(result, ProbeOutcome::Valid);
        let _ = server.join();
    }

    #[test]
    fn probe_404_is_not_an_expired_token() {
        // A 404 can come from a deleted user, a changed proxy path, or a
        // different server. It must not delete the only stored credential.
        let (url, server) = spawn_status_server("404 Not Found");
        let client = test_client(&url);
        let session = test_session(&url);
        let result = tauri::async_runtime::block_on(probe_token(&client, &session));
        assert_eq!(result, ProbeOutcome::NotFound);
        let _ = server.join();
    }

    #[test]
    fn probe_unreachable_is_transient_and_does_not_require_a_server() {
        let client = test_client("http://127.0.0.1:1");
        let session = test_session("http://127.0.0.1:1");
        let result = tauri::async_runtime::block_on(probe_token(&client, &session));
        assert_eq!(result, ProbeOutcome::Retryable(SessionReason::Unreachable));
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
    fn boot_404_keeps_the_persisted_session() {
        let (url, server) = spawn_status_server("404 Not Found");
        let (store, dir) = temp_store("boot-404");
        let session = test_session(&url);
        store.save(&session).unwrap();
        let http = emby_api::default_http_client().unwrap();
        let (boot, memory) = tauri::async_runtime::block_on(boot_session(&store, &http, "test"));
        assert_eq!(boot.status, SessionStatus::SignedOut);
        assert_eq!(boot.reason, SessionReason::ServerError);
        assert!(memory.is_none());
        assert!(store.session_path().exists(), "sign-in credentials must be retained");
        let _ = server.join();
        let _ = std::fs::remove_dir_all(dir);
    }

    #[test]
    fn boot_does_not_retry_a_server_error() {
        let (url, server) = spawn_sequence_server(&["500 Internal Server Error"]);
        let (store, dir) = temp_store("boot-no-retry");
        let session = test_session(&url);
        store.save(&session).unwrap();
        let http = emby_api::default_http_client().unwrap();
        let started = std::time::Instant::now();
        let (boot, memory) = tauri::async_runtime::block_on(boot_session(&store, &http, "test"));
        assert_eq!(boot.status, SessionStatus::SignedOut);
        assert_eq!(boot.reason, SessionReason::ServerError);
        assert!(memory.is_none());
        assert!(store.session_path().exists(), "a server error keeps the credentials");
        assert!(
            started.elapsed() < Duration::from_secs(2),
            "a 500 must fail immediately, not wait for a backoff"
        );
        let requests = server.join().unwrap();
        assert_eq!(requests.len(), 1);
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
    fn logout_uses_the_logout_endpoint_with_the_stored_token() {
        let (url, server) = spawn_status_server("200 OK");
        let client = test_client(&url);
        let session = test_session(&url);
        tauri::async_runtime::block_on(client.logout()).expect("logout request");
        let request = server.join().unwrap();
        assert!(request.starts_with("POST /Sessions/Logout "), "{request}");
        // Header *names* are case-insensitive on the wire and hyper writes them
        // lowercased, so compare the header block case-insensitively instead of
        // pinning our own capitalisation.
        let lowered = request.to_ascii_lowercase();
        assert!(
            lowered.contains(&format!("x-emby-token: {}", session.access_token)),
            "{request}"
        );
        assert!(
            lowered.contains("x-emby-authorization: mediabrowser client="),
            "{request}"
        );
    }
}
