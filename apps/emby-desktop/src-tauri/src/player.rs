//! Tauri glue for the mpv kernel.
//!
//! Everything that can be tested lives in the `emby-mpv` crate; this module is
//! intentionally thin: it owns the `PlayerSession`, fans snapshots out to
//! `Channel`s, and exposes the command contract.

use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex, MutexGuard};
use std::time::Duration;

use emby_mpv::{
    MpvLibrary, MpvOption, OpenRequest, PlayerCommand, PlayerSession, PlayerSnapshot, PlayerStats,
    SessionConfig, SnapshotAssembler, SnapshotHub, SnapshotSink, TrackKind, VideoSync,
    SHUTDOWN_TIMEOUT, STATUS_IDLE,
};
use tauri::ipc::Channel;
use tauri::State;

use crate::settings::{AppSettings, SettingsStore, VideoQuality};
use crate::video_window::VideoHost;

/// `player_open` waits at most this long for the worker's loading snapshot.
const OPEN_REPLY_TIMEOUT: Duration = Duration::from_secs(2);
/// `player_stats` waits at most this long for the mpv worker. The info panel
/// polls this only while it is open; the bound exists so a stuck mpv cannot
/// hang the request, not as an expected latency.
const STATS_TIMEOUT: Duration = Duration::from_millis(800);

/// Everything that is fixed for the lifetime of one mpv session.
///
/// libmpv only accepts these options before `mpv_initialize()`, so a session
/// can never be reconfigured in place. Comparing against this key is what makes
/// a rebuild *explicit*: it happens where the configuration actually changed and
/// says why, instead of being silently triggered by the next unrelated open.
#[derive(Debug, Clone, PartialEq)]
struct SessionKey {
    /// Quality preset; maps to an mpv profile.
    quality: VideoQuality,
    /// `http-header-fields`. Empty and absent are the same thing.
    headers: Option<Vec<String>>,
}

impl SessionKey {
    fn new(quality: VideoQuality, headers: Option<Vec<String>>) -> Self {
        // An empty list would be passed to mpv as an empty option string; treat
        // it as "no headers" so a caller sending `Some(vec![])` does not force a
        // pointless rebuild.
        let headers = headers.filter(|list| !list.is_empty());
        Self { quality, headers }
    }
}

#[derive(Debug, Clone, Copy)]
struct DesiredPlayback {
    volume: f64,
    muted: bool,
}

impl Default for DesiredPlayback {
    fn default() -> Self {
        Self {
            volume: 100.0,
            muted: false,
        }
    }
}

/// Forwards snapshots to the current `player_subscribe` channel. A new
/// subscription replaces this sink (see `PlayerService::subscribe`).
struct ChannelSink {
    channel: Channel<PlayerSnapshot>,
    alive: AtomicBool,
}

impl ChannelSink {
    fn new(channel: Channel<PlayerSnapshot>) -> Self {
        Self {
            channel,
            alive: AtomicBool::new(true),
        }
    }
}

impl SnapshotSink for ChannelSink {
    fn publish(&self, snapshot: PlayerSnapshot) {
        if !self.alive.load(Ordering::SeqCst) {
            return;
        }
        if self.channel.send(snapshot).is_err() {
            // The webview is gone (window close / reload). Stay silent for the
            // normal case and let SnapshotHub prune this sink.
            self.alive.store(false, Ordering::SeqCst);
        }
    }

    fn is_alive(&self) -> bool {
        self.alive.load(Ordering::SeqCst)
    }
}

pub struct PlayerService {
    hub: Arc<SnapshotHub>,
    host: Option<VideoHost>,
    session: Mutex<Option<PlayerSession>>,
    desired: Mutex<DesiredPlayback>,
    /// Most recent `VideoSync` the host accepted. Used to re-place the native
    /// child window after a window/monitor transition the page cannot observe
    /// reliably (fullscreen).
    last_sync: Mutex<Option<VideoSync>>,
    /// Set when a fullscreen toggle hid the video and a re-sync is owed.
    resync_pending: AtomicBool,
    shutdown_started: AtomicBool,
    /// `Some` only when `EMBY_MPV_LOG` was set at startup.
    log_path: Option<PathBuf>,
    /// App config directory; holds the optional user `mpv.conf`.
    config_dir: Option<PathBuf>,
    /// Persisted quality/autoplay/page-size preferences (`settings.json`).
    settings: Option<SettingsStore>,
    /// Immutable configuration of the running mpv session. `None` means "no
    /// session"; a mismatch against the desired key retires it.
    session_key: Mutex<Option<SessionKey>>,
}

impl PlayerService {
    pub fn new(host: Option<VideoHost>, config_dir: Option<PathBuf>) -> Self {
        let settings = config_dir
            .as_ref()
            .map(|dir| SettingsStore::new(dir.clone()));
        Self {
            hub: Arc::new(SnapshotHub::new()),
            host,
            session: Mutex::new(None),
            desired: Mutex::new(DesiredPlayback::default()),
            last_sync: Mutex::new(None),
            resync_pending: AtomicBool::new(false),
            shutdown_started: AtomicBool::new(false),
            log_path: mpv_log_path(),
            config_dir,
            settings,
            session_key: Mutex::new(None),
        }
    }

    /// Current persisted settings. A missing file, unreadable file or broken
    /// JSON yields [`AppSettings::default`] (low-overhead preset), preserving
    /// the pre-settings playback behaviour.
    pub(crate) fn current_settings(&self) -> AppSettings {
        self.settings
            .as_ref()
            .map(SettingsStore::load)
            .unwrap_or_default()
    }

    /// Saves settings.
    ///
    /// mpv options are immutable after `mpv_initialize()`, so a quality change
    /// cannot be applied to a running session. Instead of "silently rebuild on
    /// the next unrelated `player_open`", the rebuild happens **here**, where
    /// the change actually happened, and is logged with its reason. The current
    /// playback is stopped by the rebuild; the UI reloads its snapshot and the
    /// user resumes from the same position.
    pub(crate) fn save_settings(&self, settings: AppSettings) -> Result<AppSettings, String> {
        let settings = settings.normalized();
        let store = self
            .settings
            .as_ref()
            .ok_or_else(|| "无法定位应用配置目录，设置未保存".to_string())?;
        store
            .save(&settings)
            .map_err(|err| format!("无法保存设置：{err}"))?;

        // Only the session-level part of the settings matters here. Page size
        // and autoplay are read per request and must not interrupt playback.
        let live = self.lock_session_key().clone();
        if let Some(live) = live {
            if live.quality != settings.video_quality {
                self.rebuild_session(&format!(
                    "quality changed from {:?} to {:?}",
                    live.quality, settings.video_quality
                ));
            }
        }
        Ok(settings)
    }

    /// User overrides from `<config>/mpv.conf`. Read on every open so edits
    /// apply without restarting the app; a missing file or an unreadable one
    /// simply means "no overrides".
    fn user_options(&self) -> Vec<MpvOption> {
        let Some(directory) = self.config_dir.as_ref() else {
            return Vec::new();
        };
        let path = directory.join("mpv.conf");
        let text = match std::fs::read_to_string(&path) {
            Ok(text) => text,
            Err(err) if err.kind() == std::io::ErrorKind::NotFound => return Vec::new(),
            Err(err) => {
                eprintln!("[player] could not read {}: {err}", path.display());
                return Vec::new();
            }
        };
        let (options, warnings) = emby_mpv::parse_user_options(&text);
        for warning in warnings {
            eprintln!("[player] {}: {warning}", path.display());
        }
        if !options.is_empty() {
            eprintln!(
                "[player] applying {} option(s) from {}",
                options.len(),
                path.display()
            );
        }
        options
    }

    fn lock_last_sync(&self) -> MutexGuard<'_, Option<VideoSync>> {
        self.last_sync
            .lock()
            .unwrap_or_else(|poison| poison.into_inner())
    }

    fn lock_session(&self) -> MutexGuard<'_, Option<PlayerSession>> {
        self.session
            .lock()
            .unwrap_or_else(|poison| poison.into_inner())
    }

    fn lock_desired(&self) -> MutexGuard<'_, DesiredPlayback> {
        self.desired
            .lock()
            .unwrap_or_else(|poison| poison.into_inner())
    }

    fn lock_session_key(&self) -> MutexGuard<'_, Option<SessionKey>> {
        self.session_key
            .lock()
            .unwrap_or_else(|poison| poison.into_inner())
    }

    fn spawn_session(&self, key: SessionKey) -> Result<PlayerSession, String> {
        let host = self
            .host
            .as_ref()
            .ok_or_else(|| {
                "player_open: native video host is unavailable (Windows-only embedding)".to_string()
            })?;
        // DLL errors must be synchronous so `player_open` can show them.
        let library = MpvLibrary::load().map_err(|err| err.to_string())?;
        library.check_api_version().map_err(|err| err.to_string())?;
        let desired = *self.lock_desired();
        eprintln!(
            "[player] creating mpv session (quality={:?}, headers={})",
            key.quality,
            if key.headers.is_some() { "yes" } else { "no" }
        );
        let config = SessionConfig {
            host_hwnd: host.hwnd(),
            log_path: self.log_path.clone(),
            headers: key.headers.clone(),
            profile: key.quality.mpv_profile().map(str::to_string),
            extra_options: self.user_options(),
            volume: desired.volume,
            muted: desired.muted,
        };
        let session = PlayerSession::spawn(library, config, Arc::clone(&self.hub))
            .map_err(|err| format!("player_open: {err}"))?;
        *self.lock_session_key() = Some(key);
        Ok(session)
    }

    /// Retire the running mpv session so the next command creates one with the
    /// current settings. Called where the configuration changed (settings save)
    /// or where a request needs options the running session does not have.
    ///
    /// Returns whether a session was actually torn down.
    ///
    /// Blocking, but bounded by `SHUTDOWN_TIMEOUT` and short in practice (mpv
    /// normally terminates in tens of milliseconds). It is called from
    /// `save_settings`, so it must not run on a thread that owns a window —
    /// see the note in `settings_set` about making that command `(async)`.
    fn rebuild_session(&self, reason: &str) -> bool {
        // Take the session out and release the lock *before* waiting: mpv's
        // shutdown is bounded but not instant, and holding the session mutex
        // across it would queue every control command behind a settings save.
        let running = {
            let mut session = self.lock_session();
            *self.lock_session_key() = None;
            session.take()
        };
        let Some(mut running) = running else {
            return false;
        };
        eprintln!("[player] recreating mpv session: {reason}");
        if !running.shutdown(SHUTDOWN_TIMEOUT) {
            eprintln!(
                "[player] previous mpv session did not stop within {SHUTDOWN_TIMEOUT:?}; \
                 it is detached and the new session starts anyway"
            );
        }
        true
    }

    // ---- commands -----------------------------------------------------------

    pub fn open(&self, request: OpenRequest) -> Result<PlayerSnapshot, String> {
        if self.shutdown_started.load(Ordering::SeqCst) {
            return Err("player_open: the application is shutting down".to_string());
        }
        let url = request.url.trim().to_string();
        if url.is_empty() {
            return Err("player_open: empty url".to_string());
        }
        let start_seconds = request.start_seconds.filter(|s| s.is_finite() && *s > 0.0);
        // Read settings before taking the session lock so a slow disk cannot
        // block player controls; the quality preset only matters when spawning.
        let key = SessionKey::new(self.current_settings().video_quality, request.headers);

        let reply = {
            let mut session = self.lock_session();
            // Re-check under the session lock: `shutdown` may have completed
            // between the flag check above and here, and must not be raced by a
            // freshly spawned worker targeting a destroyed host window.
            if self.shutdown_started.load(Ordering::SeqCst) {
                return Err("player_open: the application is shutting down".to_string());
            }
            // mpv options are fixed before `mpv_initialize()`, so a request that
            // needs different ones can only be served by a new session. Both
            // causes are logged by `rebuild_session`/`spawn_session` above;
            // nothing is rebuilt when the key is unchanged.
            // The retire-and-respawn pair stays under the session lock on
            // purpose: releasing it between the two would let a concurrent open
            // spawn a second worker for the same host window. This only runs on
            // a real configuration change, never on an ordinary open.
            if session.is_some() && self.lock_session_key().as_ref() != Some(&key) {
                let running = session.take();
                *self.lock_session_key() = None;
                if let Some(mut running) = running {
                    eprintln!(
                        "[player] open: recreating the mpv session for the requested \
                         quality/headers"
                    );
                    if !running.shutdown(SHUTDOWN_TIMEOUT) {
                        eprintln!(
                            "[player] previous mpv session did not stop within \
                             {SHUTDOWN_TIMEOUT:?}; detaching it and starting a new one"
                        );
                    }
                }
            }
            if session.is_none() {
                *session = Some(self.spawn_session(key)?);
            }

            let (reply_tx, reply_rx) = std::sync::mpsc::channel();
            let send_result = session
                .as_ref()
                .expect("session exists after spawn")
                .send(PlayerCommand::Open {
                    url: url.clone(),
                    start_seconds,
                    reply: Some(reply_tx),
                });
            if let Err(err) = send_result {
                eprintln!("[player] open: mpv thread is gone ({err}); dropping the session");
                *session = None;
                *self.lock_session_key() = None;
                return Err(format!(
                    "player_open: mpv worker is not running ({err}); retry the open"
                ));
            }
            reply_rx
        };

        // The worker publishes the post-reset loading snapshot and replies with
        // it; the bounded wait keeps the returned `seq` consistent with the
        // channel stream (this is not the UI thread).
        //
        // A timeout is a *failure*, never a fallback to `hub.latest()`: that
        // snapshot still describes the previous file, so the UI would show the
        // old title/duration as if the new one were ready to play.
        match reply.recv_timeout(OPEN_REPLY_TIMEOUT) {
            Ok(snapshot) => Ok(snapshot),
            Err(err) => {
                eprintln!("[player] open: no snapshot reply within {OPEN_REPLY_TIMEOUT:?}: {err}");
                Err(format!(
                    "player_open: mpv did not confirm the stream within {} s ({}); retry the open",
                    OPEN_REPLY_TIMEOUT.as_secs(),
                    match err {
                        std::sync::mpsc::RecvTimeoutError::Timeout => "worker is busy",
                        std::sync::mpsc::RecvTimeoutError::Disconnected => "worker is gone",
                    }
                ))
            }
        }
    }

    /// Play/pause. Unlike the "remember for later" preferences below, this only
    /// makes sense with a loaded stream, so it reports when there is none.
    pub fn set_paused(&self, paused: bool) -> Result<(), String> {
        let session = self.lock_session();
        match session.as_ref() {
            Some(active) => active
                .send(PlayerCommand::SetPaused(paused))
                .map_err(|err| format!("player_set_paused: mpv worker is not running ({err})"))
                .map(|()| {
                    eprintln!("[player] set_paused({paused}) dispatched to mpv");
                }),
            None => Err(no_session_error("player_set_paused")),
        }
    }

    pub fn seek(&self, seconds: f64, exact: Option<bool>) -> Result<(), String> {
        if !seconds.is_finite() {
            return Err(format!("player_seek: invalid position {seconds}"));
        }
        self.dispatch(
            "player_seek",
            PlayerCommand::Seek {
                seconds: seconds.max(0.0),
                exact: exact.unwrap_or(true),
            },
        )
    }

    pub fn set_volume(&self, volume: f64) -> Result<(), String> {
        let volume = if volume.is_finite() {
            volume.clamp(0.0, 100.0)
        } else {
            100.0
        };
        self.lock_desired().volume = volume;
        let session = self.lock_session();
        match session.as_ref() {
            Some(active) => active
                .send(PlayerCommand::SetVolume(volume))
                .map_err(|err| err.to_string()),
            None => {
                // No mpv yet: remember it and reflect it in the snapshot.
                self.hub.update(|snapshot| snapshot.volume = volume);
                Ok(())
            }
        }
    }

    pub fn set_muted(&self, muted: bool) -> Result<(), String> {
        self.lock_desired().muted = muted;
        let session = self.lock_session();
        match session.as_ref() {
            Some(active) => active
                .send(PlayerCommand::SetMuted(muted))
                .map_err(|err| err.to_string()),
            None => {
                self.hub.update(|snapshot| snapshot.muted = muted);
                Ok(())
            }
        }
    }

    pub fn set_track(&self, kind: &str, id: i64) -> Result<(), String> {
        let Some(kind) = TrackKind::parse(kind) else {
            return Err(format!(
                "player_set_track: unknown track kind {kind:?} (expected \"audio\" or \"subtitle\")"
            ));
        };
        self.dispatch("player_set_track", PlayerCommand::SetTrack { kind, id })
    }

    pub fn stop(&self) -> Result<(), String> {
        // The UI leaves the player page with `player_stop` (+ a hidden
        // `video_sync`), so the native window must go away even if the worker
        // is already gone or never started.
        self.hide_video();
        if let Some(active) = self.lock_session().as_ref() {
            // Best effort: if the worker already died the idle snapshot below
            // still puts the UI back into a sane state.
            let _ = active.send(PlayerCommand::Stop);
        }
        // The idle shape is owned by the crate that also owns the state machine
        // (`SnapshotAssembler::idle_snapshot`), so this cannot drift from
        // `reset_to_idle` when a field is added. Only the desired volume/mute
        // are carried over: stopping must not make the bar jump to 100 %.
        let desired = *self.lock_desired();
        let idle = SnapshotAssembler::idle_snapshot(desired.volume, desired.muted);
        self.hub.publish(idle);
        Ok(())
    }

    pub fn snapshot(&self) -> PlayerSnapshot {
        self.hub.latest()
    }

    /// Live mpv statistics; fails when no session exists yet.
    ///
    /// The 800 ms wait for mpv happens **after** the session lock is released:
    /// holding it across the wait queued every control command (play/pause,
    /// seek) behind the info panel's poll, which is what made the bar feel
    /// stuck while the panel was open. `Command::Stats` only needs the channel
    /// sender, so the lock is only held long enough to clone it.
    pub fn stats(&self) -> Result<PlayerStats, String> {
        let sender = {
            let session = self.lock_session();
            match session.as_ref() {
                Some(active) => active.command_sender(),
                None => return Err(no_session_error("player_stats")),
            }
        };
        let (tx, rx) = std::sync::mpsc::channel();
        sender
            .send(PlayerCommand::Stats(tx))
            .map_err(|err| format!("player_stats: mpv worker is not running ({err})"))?;
        rx.recv_timeout(STATS_TIMEOUT)
            .map_err(|err| format!("player_stats: mpv did not answer within {STATS_TIMEOUT:?} ({err})"))
    }

    pub fn subscribe(&self, channel: Channel<PlayerSnapshot>) -> Result<(), String> {
        // Subscribing is idempotent by design: the UI re-subscribes on every
        // visit to the player page and there is no unsubscribe command, so the
        // newest channel replaces all previous ones (and receives the current
        // snapshot immediately).
        self.hub.set_sink(Arc::new(ChannelSink::new(channel)));
        Ok(())
    }

    pub fn video_sync(&self, request: VideoSync) -> Result<(), String> {
        match &self.host {
            Some(host) => {
                // `false` only means "stale seq" or "window destroyed"; not an
                // IPC error the UI needs to handle.
                if host.push_sync(request) {
                    *self.lock_last_sync() = Some(request);
                }
                Ok(())
            }
            None => Err("video_sync: native video host is unavailable on this platform".to_string()),
        }
    }

    pub fn hide_video(&self) {
        if let Some(host) = &self.host {
            let _ = host.hide();
        }
    }

    /// Toggle borderless fullscreen with the surrounding window choreography.
    ///
    /// Order matters on Windows:
    /// 1. read the current state first (a blocking getter; it must not run
    ///    after the video was hidden, or a failure would leave a black page),
    /// 2. really hide the owned overlay window (a surviving owned top-level
    ///    window keeps a borderless window from being treated as fullscreen by
    ///    the DWM and ends up floating over the video),
    /// 3. hide the native child, toggle, and let `main`'s `Resized` handler
    ///    resume the overlay and re-place the child once the new client size
    ///    is known.
    pub fn toggle_fullscreen(
        &self,
        app: &tauri::AppHandle,
        window: &tauri::WebviewWindow,
    ) -> Result<bool, String> {
        let was_fullscreen = window.is_fullscreen().map_err(|err| err.to_string())?;
        let next = !was_fullscreen;

        crate::overlay::suspend(app);
        self.hide_video();
        // Set before the toggle: the window's `Resized` event can be delivered
        // while `set_fullscreen` is still running, and it is what consumes this
        // flag to re-place the video once the new client size exists.
        self.resync_pending.store(true, Ordering::SeqCst);

        if let Err(err) = window.set_fullscreen(next) {
            // The window did not change state: put the overlay back at once so
            // the failure does not also hide the controls, and drop the pending
            // re-sync (the old rect is still valid).
            self.resync_pending.store(false, Ordering::SeqCst);
            crate::overlay::resume(app);
            return Err(err.to_string());
        }
        eprintln!("[player] fullscreen -> {next}");
        Ok(next)
    }

    /// Re-apply the last accepted `VideoSync` after a fullscreen transition.
    ///
    /// Called from the window's `Resized`/`Moved` handler once the new client
    /// size exists. Idempotent and cheap when nothing is owed. `seq` is pushed
    /// as `0` on purpose: `video_window` treats it as "the sender restarted its
    /// counter" and resets its latch, so this can never be rejected as stale
    /// after the synthetic hide sequence that `VideoHost::hide()` consumed.
    pub fn resync_video(&self) {
        if !self.resync_pending.swap(false, Ordering::SeqCst) {
            return;
        }
        let Some(mut sync) = *self.lock_last_sync() else {
            return;
        };
        let Some(host) = &self.host else {
            return;
        };
        sync.seq = 0;
        let posted = host.push_sync(sync);
        eprintln!("[player] video re-sync after fullscreen: posted={posted}");
    }

    /// Stop mpv (quit + join, 2 s timeout) and then destroy the host window.
    /// Must be called on the main thread. Idempotent.
    pub fn shutdown(&self) {
        if self.shutdown_started.swap(true, Ordering::SeqCst) {
            return;
        }
        let session = self.lock_session().take();
        *self.lock_session_key() = None;
        let mut clean = true;
        if let Some(mut session) = session {
            clean = session.shutdown(SHUTDOWN_TIMEOUT);
            if !clean {
                // The mpv thread is stuck in terminate_destroy; keep the host
                // window alive (parent of mpv's child) and let process exit
                // reclaim it instead of destroying it under a live mpv.
                eprintln!(
                    "[player] mpv did not stop within {:?}; leaving the video host window for process teardown",
                    SHUTDOWN_TIMEOUT
                );
            }
        }
        if clean {
            if let Some(host) = &self.host {
                host.destroy();
            }
        }
        self.hub.update(|snapshot| {
            snapshot.status = STATUS_IDLE.to_string();
            snapshot.buffering = false;
        });
        // In-flight WM_APP+1 messages are harmless: VideoHost::push_sync and
        // the window procedure both check the destroyed flag / HWND validity.
    }

    /// Queue a command that is only meaningful for a loaded stream.
    ///
    /// One rule for every such command (seek, tracks, ...): no session is an
    /// error with the same wording, so the UI never gets `Ok(())` for a command
    /// that did nothing. The old split — `set_paused` errored while `seek` and
    /// `set_track` silently succeeded — is what made a dead session look alive.
    ///
    /// Volume/mute deliberately do *not* go through here: they are preferences
    /// that outlive a session (see `set_volume`/`set_muted`).
    fn dispatch(&self, command_name: &str, command: PlayerCommand) -> Result<(), String> {
        let session = self.lock_session();
        match session.as_ref() {
            Some(active) => active
                .send(command)
                .map_err(|err| format!("{command_name}: mpv worker is not running ({err})")),
            None => Err(no_session_error(command_name)),
        }
    }
}

/// One wording for "this command needs an open stream".
fn no_session_error(command_name: &str) -> String {
    format!("{command_name}: no active mpv session (open a stream first)")
}

/// The libmpv file log is off unless `EMBY_MPV_LOG` is truthy at startup:
/// mpv logs stream URLs, and Emby stream URLs carry `api_key`.
fn mpv_log_path() -> Option<PathBuf> {
    let value = std::env::var(emby_mpv::options::LOG_ENV_VAR).ok();
    if !emby_mpv::options::log_enabled_from(value.as_deref()) {
        return None;
    }
    let mut directory = std::env::temp_dir();
    directory.push("emby-desktop");
    if let Err(err) = std::fs::create_dir_all(&directory) {
        eprintln!(
            "[player] could not create log directory {}: {err}",
            directory.display()
        );
    }
    let path = directory.join(format!("mpv-{}.log", std::process::id()));
    // The path is all we print; the file itself contains api_key-bearing URLs.
    eprintln!("[player] mpv file log enabled: {}", path.display());
    Some(path)
}

// ---- Tauri commands ----------------------------------------------------------
// Argument names are converted to camelCase by Tauri (`on_event` -> `onEvent`,
// `req` -> `req`, ...). Wire types serialize with `rename_all = "camelCase"`.

#[tauri::command]
pub fn video_sync(
    app: tauri::AppHandle,
    req: VideoSync,
    state: State<'_, PlayerService>,
) -> Result<(), String> {
    // The host owns mpv's embedding; the overlay mirrors the same rectangle so
    // the transparent controls window can follow it. Feed the overlay even if
    // the host reports a stale seq / missing platform support: it has its own
    // latch and simply stays where it is.
    let result = state.video_sync(req);
    crate::overlay::on_video_sync(&app, req);
    result
}

// Runs on Tauri's thread pool: `open` waits (bounded, typically < 50 ms) for
// the mpv worker's loading snapshot and must not block the main thread.
#[tauri::command(async)]
pub fn player_open(
    req: OpenRequest,
    state: State<'_, PlayerService>,
) -> Result<PlayerSnapshot, String> {
    state.open(req)
}

#[tauri::command]
pub fn player_set_paused(paused: bool, state: State<'_, PlayerService>) -> Result<(), String> {
    state.set_paused(paused)
}

#[tauri::command]
pub fn player_seek(
    seconds: f64,
    exact: Option<bool>,
    state: State<'_, PlayerService>,
) -> Result<(), String> {
    state.seek(seconds, exact)
}

#[tauri::command]
pub fn player_set_volume(volume: f64, state: State<'_, PlayerService>) -> Result<(), String> {
    state.set_volume(volume)
}

#[tauri::command]
pub fn player_set_muted(muted: bool, state: State<'_, PlayerService>) -> Result<(), String> {
    state.set_muted(muted)
}

#[tauri::command]
pub fn player_set_track(
    kind: String,
    id: i64,
    state: State<'_, PlayerService>,
) -> Result<(), String> {
    state.set_track(&kind, id)
}

#[tauri::command]
pub fn player_stop(state: State<'_, PlayerService>) -> Result<(), String> {
    state.stop()
}

#[tauri::command]
pub fn player_snapshot(state: State<'_, PlayerService>) -> Result<PlayerSnapshot, String> {
    Ok(state.snapshot())
}

// Runs on Tauri's thread pool: the 800 ms bound on mpv's reply must never be
// paid on the main thread, which is shared with the webview and every window
// message (same reasoning as `player_open`).
#[tauri::command(async)]
pub fn player_stats(state: State<'_, PlayerService>) -> Result<PlayerStats, String> {
    state.stats()
}

#[tauri::command]
pub fn player_toggle_fullscreen(
    app: tauri::AppHandle,
    window: tauri::WebviewWindow,
    state: State<'_, PlayerService>,
) -> Result<bool, String> {
    state.toggle_fullscreen(&app, &window)
}

#[tauri::command]
pub fn player_subscribe(
    on_event: Channel<PlayerSnapshot>,
    state: State<'_, PlayerService>,
) -> Result<(), String> {
    state.subscribe(on_event)
}
