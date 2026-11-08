//! Tauri bridge for `emby-api`.
//!
//! Responsibilities:
//! - own the one shared `reqwest::Client`,
//! - lazily restore `session.json` from the app config dir,
//! - drop the in-memory *and* on-disk session on 401/403,
//! - expose the camelCase command contract consumed by the frontend.
//!
//! All commands are `async fn`: Tauri runs them on its tokio runtime, never on
//! the UI thread, and the `reqwest` calls are non-blocking. File I/O for
//! `session.json` is a few hundred bytes, so it is done inline instead of being
//! pushed through `spawn_blocking`.
//!
//! Contract note: commands return `Result<T, String>` (Tauri rejects the
//! promise on error) so the frontend can decide whether to ignore a failed
//! progress report. Success values match the TypeScript types exactly.

use emby_api::{
    ApiError, EmbyClient, EmbyImageRequest, EmbyItem, EmbyItemsPage, EmbyItemsRequest,
    EmbyLoginRequest, EmbyPlaybackRequest, EmbyPlaybackTarget, EmbyReportRequest, EmbySession,
    EmbyView, SessionStore, StoredSession,
};
use tauri::async_runtime::Mutex;
use tauri::{AppHandle, Manager, State};

/// Message shown when the server no longer accepts the stored token.
const AUTH_EXPIRED: &str = "登录已失效，请重新登录";

/// Managed state for every `emby_*` command.
pub struct EmbyService {
    http: reqwest::Client,
    store: SessionStore,
    device_name: String,
    session: Mutex<SessionState>,
}

/// `loaded` distinguishes "not read from disk yet" from "read, no session".
#[derive(Default)]
struct SessionState {
    loaded: bool,
    session: Option<StoredSession>,
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
            session: Mutex::new(SessionState::default()),
        })
    }

    /// Restores the session from disk exactly once.
    async fn stored_session(&self) -> Option<StoredSession> {
        let mut state = self.session.lock().await;
        if !state.loaded {
            state.session = match self.store.load() {
                Ok(session) => session,
                Err(err) => {
                    // Messages never contain the token (see emby-api::session).
                    eprintln!("[emby] session restore failed: {err}");
                    None
                }
            };
            state.loaded = true;
        }
        state.session.clone()
    }

    async fn require_session(&self) -> Result<StoredSession, String> {
        self.stored_session()
            .await
            .ok_or_else(|| "尚未登录 Emby 服务器".to_string())
    }

    async fn remember(&self, session: StoredSession) {
        let mut state = self.session.lock().await;
        state.loaded = true;
        state.session = Some(session);
    }

    /// Clears memory and disk. Returns an error when the file could not be
    /// removed, so a "logout" never silently leaves the token on disk.
    async fn forget(&self) -> Result<(), String> {
        {
            let mut state = self.session.lock().await;
            state.loaded = true;
            state.session = None;
        }
        self.store.clear().map_err(|err| err.to_string())
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

    /// Converts an API failure into a command error; 401/403 also drops the
    /// session because the token is useless after that.
    async fn fail(&self, err: ApiError) -> String {
        if err.is_auth() {
            if let Err(clear_err) = self.forget().await {
                eprintln!("[emby] could not clear the expired session: {clear_err}");
            }
            AUTH_EXPIRED.to_string()
        } else {
            err.to_string()
        }
    }
}

#[tauri::command]
pub async fn emby_login(
    req: EmbyLoginRequest,
    state: State<'_, EmbyService>,
) -> Result<EmbySession, String> {
    let server_url = emby_api::normalize_server_url(&req.server).map_err(|err| err.to_string())?;
    if req.username.trim().is_empty() {
        return Err("请输入用户名".to_string());
    }

    let device_id = state.store.device_id().map_err(|err| err.to_string())?;
    let client = EmbyClient::new(
        state.http.clone(),
        server_url.clone(),
        device_id.clone(),
        state.device_name.clone(),
    );
    // A failed login must not clear an existing valid session: report the error
    // as-is (wrong password -> readable message from emby-api).
    let outcome = client
        .login(&req.username, &req.password)
        .await
        .map_err(|err| err.to_string())?;

    let session = StoredSession {
        server_url,
        access_token: outcome.access_token,
        user_id: outcome.user_id,
        user_name: outcome.user_name,
        server_name: outcome.server_name,
        device_id,
    };
    state
        .store
        .save(&session)
        .map_err(|err| format!("登录成功，但无法保存会话：{err}"))?;
    state.remember(session.clone()).await;
    Ok(session.public())
}

#[tauri::command]
pub async fn emby_logout(state: State<'_, EmbyService>) -> Result<(), String> {
    state.forget().await
}

/// Restores the persisted session on first call. Never returns the token.
#[tauri::command]
pub async fn emby_session(
    state: State<'_, EmbyService>,
) -> Result<Option<EmbySession>, String> {
    Ok(state.stored_session().await.map(|session| session.public()))
}

#[tauri::command]
pub async fn emby_views(state: State<'_, EmbyService>) -> Result<Vec<EmbyView>, String> {
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
) -> Result<EmbyItemsPage, String> {
    let session = state.require_session().await?;
    let client = state.client(&session);
    match client.items(&session.user_id, &req).await {
        Ok(page) => Ok(page),
        Err(err) => Err(state.fail(err).await),
    }
}

#[tauri::command]
pub async fn emby_item(
    id: String,
    state: State<'_, EmbyService>,
) -> Result<EmbyItem, String> {
    let session = state.require_session().await?;
    let client = state.client(&session);
    match client.item(&session.user_id, &id).await {
        Ok(item) => Ok(item),
        Err(err) => Err(state.fail(err).await),
    }
}

#[tauri::command]
pub async fn emby_playback_target(
    req: EmbyPlaybackRequest,
    state: State<'_, EmbyService>,
) -> Result<EmbyPlaybackTarget, String> {
    let session = state.require_session().await?;
    let client = state.client(&session);
    match client.playback_target(&session.user_id, &req).await {
        Ok(target) => Ok(target),
        Err(err) => Err(state.fail(err).await),
    }
}

/// Progress reporting failures are returned as `Err` so the UI may ignore them;
/// a 401 still invalidates the session.
#[tauri::command]
pub async fn emby_report_playback(
    req: EmbyReportRequest,
    state: State<'_, EmbyService>,
) -> Result<(), String> {
    let session = state.require_session().await?;
    let client = state.client(&session);
    match client.report_playback(&req).await {
        Ok(()) => Ok(()),
        Err(err) => Err(state.fail(err).await),
    }
}

/// Returns an absolute image URL that already carries `api_key`; the frontend
/// can put it directly into `<img src>`.
#[tauri::command]
pub async fn emby_image_url(
    req: EmbyImageRequest,
    state: State<'_, EmbyService>,
) -> Result<String, String> {
    let session = state.require_session().await?;
    let client = state.client(&session);
    client.image_url(&req).map_err(|err| err.to_string())
}
