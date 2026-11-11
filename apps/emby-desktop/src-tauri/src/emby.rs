//! Tauri bridge for `emby-api`.
//!
//! Responsibilities:
//! - own the one shared `emby_api::HttpClient`,
//! - lazily restore `session.json` from the app config dir (retrying a
//!   transient disk error instead of treating it as a logout),
//! - re-check an auth failure once before dropping the persisted session,
//! - expose the camelCase command contract consumed by the frontend.
//!
//! All commands are `async fn`: Tauri runs them on its tokio runtime, never on
//! the UI thread, and the HTTP calls are non-blocking. File I/O for
//! `session.json` is a few hundred bytes, so it is done inline instead of being
//! pushed through `spawn_blocking`.
//!
//! Contract note: commands return `Result<T, String>` (Tauri rejects the
//! promise on error) so the frontend can decide whether to ignore a failed
//! progress report. Success values match the TypeScript types exactly.

use std::time::Duration;

use emby_api::{
    ApiError, EmbyClient, EmbyImageRequest, EmbyItem, EmbyItemsPage, EmbyItemsRequest,
    EmbyLoginRequest, EmbyPlaybackRequest, EmbyPlaybackTarget, EmbyReportRequest,
    EmbyResumeRequest, EmbySession, EmbyView, SessionStore, StoredSession,
};
use tauri::async_runtime::Mutex;
use tauri::{AppHandle, Manager, State};

/// Message shown when the server no longer accepts the stored token.
const AUTH_EXPIRED: &str = "登录已过期，请重新登录";

/// Page size used when `emby_resume` does not receive a limit.
const DEFAULT_RESUME_LIMIT: u32 = 16;
const DEFAULT_SIMILAR_LIMIT: u32 = 12;
const DEFAULT_PERSON_LIMIT: u32 = 60;

/// Managed state for every `emby_*` command.
pub struct EmbyService {
    http: emby_api::HttpClient,
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

    /// Restores the session from disk, retrying a transient I/O failure a few
    /// times. A failure is *not* cached as "logged out": a slow/roaming
    /// profile or antivirus lock during a Windows boot must not force the user
    /// through the login page for the rest of the run.
    async fn stored_session(&self) -> Result<Option<StoredSession>, String> {
        {
            let state = self.session.lock().await;
            if state.loaded {
                return Ok(state.session.clone());
            }
        }

        const ATTEMPTS: usize = 3;
        let mut last_error = String::new();
        for attempt in 1..=ATTEMPTS {
            match self.store.load() {
                Ok(maybe_session) => {
                    let mut state = self.session.lock().await;
                    // Login can win the race while the disk read was in
                    // flight; never overwrite the fresh in-memory session.
                    if state.loaded {
                        return Ok(state.session.clone());
                    }
                    if let Some(session) = maybe_session {
                        state.loaded = true;
                        state.session = Some(session.clone());
                        return Ok(Some(session));
                    }
                    // Do not cache "file missing": after a Windows boot the
                    // profile can become readable a moment later, and the
                    // frontend retries `emby_session` once.
                    return Ok(None);
                }
                Err(err) => {
                    // Messages never contain the token (see emby-api::session).
                    last_error = err.to_string();
                    if attempt < ATTEMPTS {
                        std::thread::sleep(Duration::from_millis(150));
                    }
                }
            }
        }
        Err(format!("无法读取本地登录会话：{last_error}"))
    }

    async fn require_session(&self) -> Result<StoredSession, String> {
        self.stored_session()
            .await?
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

    /// Converts an API failure into a command error. A 401/403 is re-checked
    /// once with a cheap authenticated request before the persisted session is
    /// removed: a transient auth failure while the Emby server/proxy is still
    /// starting (typical right after a machine reboot) must not log the user
    /// out. Only a second auth failure clears the session; a network failure
    /// during the re-check keeps it.
    async fn fail(&self, err: ApiError) -> String {
        if !err.is_auth() {
            return err.to_string();
        }

        let session = match self.stored_session().await {
            Ok(Some(session)) => session,
            Ok(None) => return AUTH_EXPIRED.to_string(),
            Err(load_err) => return load_err,
        };

        // Cheap, authenticated request that also works for restricted users.
        let client = self.client(&session);
        let retry = client.views(&session.user_id).await;
        match retry {
            Ok(_) => "Emby 暂时拒绝了请求，请稍后重试".to_string(),
            Err(retry_err) if retry_err.is_auth() => {
                if let Err(clear_err) = self.forget().await {
                    eprintln!("[emby] could not clear the expired session: {clear_err}");
                }
                AUTH_EXPIRED.to_string()
            }
            Err(_) => "无法确认 Emby 登录状态，请稍后重试".to_string(),
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
    let session = state.stored_session().await?;
    Ok(session.map(|session| session.public()))
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

/// Continue-watching list from Emby's official resume endpoint. The server
/// decides the order; `limit` defaults to `DEFAULT_RESUME_LIMIT`.
#[tauri::command]
pub async fn emby_resume(
    req: EmbyResumeRequest,
    state: State<'_, EmbyService>,
) -> Result<EmbyItemsPage, String> {
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
) -> Result<EmbyItem, String> {
    let session = state.require_session().await?;
    let client = state.client(&session);
    match client.item(&session.user_id, &id).await {
        Ok(item) => Ok(item),
        Err(err) => Err(state.fail(err).await),
    }
}

/// "More like this" row of the detail page.
#[tauri::command]
pub async fn emby_similar(
    id: String,
    limit: Option<u32>,
    state: State<'_, EmbyService>,
) -> Result<EmbyItemsPage, String> {
    let session = state.require_session().await?;
    let client = state.client(&session);
    let limit = limit.unwrap_or(DEFAULT_SIMILAR_LIMIT).clamp(1, 50);
    match client.similar(&session.user_id, &id, limit).await {
        Ok(page) => Ok(page),
        Err(err) => Err(state.fail(err).await),
    }
}

/// Works featuring a person (actor/director) from
/// `GET /Persons/{personId}/Items`. `limit` defaults to 60 and is clamped to
/// a sane page size; the item shape matches `emby_items`.
#[tauri::command]
pub async fn emby_person_items(
    person_id: String,
    limit: Option<u32>,
    state: State<'_, EmbyService>,
) -> Result<EmbyItemsPage, String> {
    let session = state.require_session().await?;
    let client = state.client(&session);
    let limit = limit.unwrap_or(DEFAULT_PERSON_LIMIT).clamp(1, 200);
    match client.person_items(&session.user_id, &person_id, limit).await {
        Ok(page) => Ok(page),
        Err(err) => Err(state.fail(err).await),
    }
}

/// Marks an item watched (`played = true`) or unwatched
/// (`POST` / `DELETE /Users/{userId}/PlayedItems/{itemId}`).
#[tauri::command]
pub async fn emby_set_played(
    id: String,
    played: bool,
    state: State<'_, EmbyService>,
) -> Result<(), String> {
    let session = state.require_session().await?;
    let client = state.client(&session);
    match client.set_played(&session.user_id, &id, played).await {
        Ok(()) => Ok(()),
        Err(err) => Err(state.fail(err).await),
    }
}

/// Adds or removes the user's favorite mark
/// (`POST` / `DELETE /Users/{userId}/FavoriteItems/{itemId}`).
#[tauri::command]
pub async fn emby_set_favorite(
    id: String,
    favorite: bool,
    state: State<'_, EmbyService>,
) -> Result<(), String> {
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
