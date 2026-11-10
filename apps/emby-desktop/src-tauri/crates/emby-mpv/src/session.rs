//! The dedicated mpv thread.
//!
//! Threading contract (see the project notes):
//! * exactly one thread owns the `mpv_handle`: create → initialize →
//!   `mpv_wait_event` loop → `mpv_terminate_destroy`
//! * the Tauri layer only sends `PlayerCommand`s over `mpsc` and reads
//!   snapshots from [`SnapshotHub`]; it never calls into mpv
//! * `mpv_terminate_destroy` is invoked outside the wait loop, after the
//!   `quit` command and the `MPV_EVENT_SHUTDOWN` event (or a 2 s timeout).

use std::ffi::{CStr, CString};
use std::os::raw::{c_char, c_int, c_void};
use std::path::PathBuf;
use std::sync::mpsc::{self, Receiver, RecvTimeoutError, Sender, TryRecvError};
use std::sync::Arc;
use std::thread::{self, JoinHandle};
use std::time::{Duration, Instant};

use crate::error::MpvError;
use crate::ffi::*;
use crate::hub::SnapshotHub;
use crate::loader::MpvLibrary;
use crate::options::{build_initial_options, MpvOption};
use crate::state::SnapshotAssembler;
use crate::types::{
    PlayerCommand, PlayerStatus, PlayerTrack, TrackKind, STATUS_ERROR, STATUS_IDLE,
};

/// `mpv_wait_event` polling interval: keeps command latency low without busy
/// spinning.
pub const EVENT_TIMEOUT_SECS: f64 = 0.02;
/// While playing, push a snapshot even if no property changed (5 Hz).
pub const SNAPSHOT_INTERVAL: Duration = Duration::from_millis(200);
/// How long `quit` may take to produce `MPV_EVENT_SHUTDOWN`.
pub const SHUTDOWN_TIMEOUT: Duration = Duration::from_secs(2);
/// Upper bound of events drained per loop iteration so commands are not
/// starved by a property-change burst.
const MAX_EVENTS_PER_TICK: usize = 512;

/// Properties observed after initialization. Formats must match the mpv
/// property documentation, otherwise the value is reported as
/// `MPV_FORMAT_NONE` and we simply ignore it.
const OBSERVED: &[(&str, MpvFormat)] = &[
    ("pause", MPV_FORMAT_FLAG),
    ("time-pos", MPV_FORMAT_DOUBLE),
    ("duration", MPV_FORMAT_DOUBLE),
    ("volume", MPV_FORMAT_DOUBLE),
    ("mute", MPV_FORMAT_FLAG),
    ("width", MPV_FORMAT_INT64),
    ("height", MPV_FORMAT_INT64),
    ("eof-reached", MPV_FORMAT_FLAG),
    ("idle-active", MPV_FORMAT_FLAG),
    ("paused-for-cache", MPV_FORMAT_FLAG),
    // Low-level notification only; the track list is re-read on the mpv thread
    // with mpv_get_property_string, never from the UI thread.
    ("track-list", MPV_FORMAT_NONE),
];

#[derive(Debug, Clone)]
pub struct SessionConfig {
    /// Parent `HWND` for `--wid` (decimal in the mpv option).
    pub host_hwnd: u64,
    /// Opt-in mpv log file (`EMBY_MPV_LOG`); `None` means no file log. When
    /// set, it contains stream URLs and therefore their `api_key`.
    pub log_path: Option<PathBuf>,
    /// Only used when this config creates a fresh handle (pre-initialize).
    pub headers: Option<Vec<String>>,
    /// User overrides from `mpv.conf`; applied after the built-in options.
    pub extra_options: Vec<MpvOption>,
    /// Volume 0..100 restored from the UI.
    pub volume: f64,
    pub muted: bool,
}

/// Handle to a running mpv thread.
pub struct PlayerSession {
    tx: Sender<PlayerCommand>,
    join: Option<JoinHandle<()>>,
    done: Receiver<()>,
    hub: Arc<SnapshotHub>,
}

impl PlayerSession {
    /// Load nothing here: `MpvLibrary` must already be loaded (so DLL errors
    /// can be returned synchronously from `player_open`).
    pub fn spawn(
        library: MpvLibrary,
        config: SessionConfig,
        hub: Arc<SnapshotHub>,
    ) -> Result<Self, MpvError> {
        let (tx, rx) = mpsc::channel();
        let (done_tx, done_rx) = mpsc::channel();
        let worker_hub = Arc::clone(&hub);
        let join = thread::Builder::new()
            .name("emby-mpv".to_string())
            .spawn(move || {
                run(library, config, worker_hub, rx);
                // If `run` panicked, this send never happens and `shutdown`
                // treats the disconnected channel as "thread finished".
                let _ = done_tx.send(());
            })
            .map_err(|err| MpvError::ThreadSpawn(err.to_string()))?;

        Ok(Self {
            tx,
            join: Some(join),
            done: done_rx,
            hub,
        })
    }

    pub fn hub(&self) -> &Arc<SnapshotHub> {
        &self.hub
    }

    pub fn send(&self, command: PlayerCommand) -> Result<(), MpvError> {
        self.tx.send(command).map_err(|_| MpvError::NotStarted)
    }

    /// Ask the worker to quit, wait up to `timeout`, then join.
    ///
    /// Returns `false` when the worker did not finish in time; the thread is
    /// then detached (process exit will clean it up) instead of blocking the
    /// UI thread indefinitely.
    pub fn shutdown(&mut self, timeout: Duration) -> bool {
        let _ = self.tx.send(PlayerCommand::Shutdown);
        let finished = match self.done.recv_timeout(timeout) {
            Ok(()) => true,
            Err(RecvTimeoutError::Disconnected) => true,
            Err(RecvTimeoutError::Timeout) => false,
        };
        if finished {
            if let Some(join) = self.join.take() {
                let _ = join.join();
            }
        } else {
            // Detach: the worker is stuck in `mpv_terminate_destroy` (usually a
            // GPU driver hang). Blocking `Drop` for another 2.5 s on the main
            // thread during app exit buys nothing.
            self.join = None;
        }
        finished
    }
}

impl Drop for PlayerSession {
    fn drop(&mut self) {
        if self.join.is_some() {
            let _ = self.shutdown(SHUTDOWN_TIMEOUT + Duration::from_millis(500));
        }
    }
}

/// Worker entry point. Owns the library (unloaded after the mpv handle died).
fn run(
    library: MpvLibrary,
    config: SessionConfig,
    hub: Arc<SnapshotHub>,
    rx: Receiver<PlayerCommand>,
) {
    if let Err(err) = run_session(&library, &config, &hub, &rx) {
        eprintln!("[emby-mpv] session failed: {err}");
        hub.update(|snapshot| {
            snapshot.status = STATUS_ERROR.to_string();
            snapshot.buffering = false;
            snapshot.error = Some(err.to_string());
        });
    }
    // `library` is dropped after this point. `run_session` owns `MpvCore`,
    // whose Drop calls `mpv_terminate_destroy` before we get here.
}

fn run_session(
    library: &MpvLibrary,
    config: &SessionConfig,
    hub: &Arc<SnapshotHub>,
    rx: &Receiver<PlayerCommand>,
) -> Result<(), MpvError> {
    let mut core = MpvCore::initialize(library, config)?;
    core.observe_all();

    let mut assembler = SnapshotAssembler::new();
    assembler.set_volume(config.volume);
    assembler.set_muted(config.muted);
    hub.update(|snapshot| {
        snapshot.volume = config.volume.clamp(0.0, 100.0);
        snapshot.muted = config.muted;
    });

    let mut last_emit = Instant::now();
    let mut shutdown_deadline: Option<Instant> = None;
    let mut shutdown_event_seen = false;

    loop {
        // ---- commands -------------------------------------------------------
        for _ in 0..64 {
            match rx.try_recv() {
                Ok(PlayerCommand::Shutdown) => {
                    core.request_quit();
                    shutdown_deadline = Some(Instant::now() + SHUTDOWN_TIMEOUT);
                }
                Ok(PlayerCommand::Open {
                    url,
                    start_seconds,
                    reply,
                }) => {
                    if let Err(err) = handle_open(&core, &mut assembler, url, start_seconds) {
                        eprintln!("[emby-mpv] open failed: {err}");
                        assembler.set_error(err.to_string());
                    }
                    // Publish synchronously: `player_open` waits for this reply
                    // so the returned snapshot cannot be older than a queued
                    // event from the previous file.
                    let snapshot = hub.publish(assembler.build());
                    assembler.clear_dirty();
                    if let Some(reply) = reply {
                        let _ = reply.send(snapshot);
                    }
                }
                Ok(PlayerCommand::Snapshot(reply)) => {
                    let _ = reply.send(hub.latest());
                }
                Ok(command) => {
                    if let Err(err) = handle_command(&core, &mut assembler, command) {
                        eprintln!("[emby-mpv] command failed: {err}");
                        assembler.mark_dirty();
                    }
                }
                Err(TryRecvError::Empty) => break,
                Err(TryRecvError::Disconnected) => {
                    // Owner vanished; play out mpv's shutdown and exit. Only
                    // request `quit` once, otherwise the deadline keeps being
                    // pushed forward and the loop never exits.
                    if shutdown_deadline.is_none() {
                        core.request_quit();
                        shutdown_deadline = Some(Instant::now() + SHUTDOWN_TIMEOUT);
                    }
                    break;
                }
            }
        }

        // ---- events (coalesce a burst into one snapshot) --------------------
        let mut processed = 0usize;
        loop {
            let timeout = if processed == 0 { EVENT_TIMEOUT_SECS } else { 0.0 };
            let Some(frame) = core.wait_event(timeout) else {
                break;
            };
            processed += 1;
            if let EventOutcome::Shutdown = handle_event(&core, &mut assembler, &frame) {
                shutdown_event_seen = true;
                shutdown_deadline.get_or_insert_with(Instant::now);
            }
            if processed >= MAX_EVENTS_PER_TICK {
                break;
            }
        }

        // ---- snapshot publishing -------------------------------------------
        let now = Instant::now();
        let periodic = assembler.is_playing() && now.duration_since(last_emit) >= SNAPSHOT_INTERVAL;
        if assembler.is_dirty() || periodic {
            assembler.clear_dirty();
            hub.publish(assembler.build());
            last_emit = now;
        }

        // ---- shutdown -------------------------------------------------------
        if shutdown_event_seen {
            break;
        }
        if let Some(deadline) = shutdown_deadline {
            if now >= deadline {
                eprintln!("[emby-mpv] mpv did not shut down within {SHUTDOWN_TIMEOUT:?}; forcing terminate");
                break;
            }
        }
    }

    // Outside the wait loop, exactly once (Drop is a no-op afterwards).
    core.terminate();
    hub.update(|snapshot| {
        snapshot.status = STATUS_IDLE.to_string();
        snapshot.buffering = false;
    });
    Ok(())
}

// ---- command handling ---------------------------------------------------------------

fn handle_command(
    core: &MpvCore<'_>,
    assembler: &mut SnapshotAssembler,
    command: PlayerCommand,
) -> Result<(), MpvError> {
    match command {
        PlayerCommand::Open { .. } => Ok(()), // handled inline, see run_session
        PlayerCommand::SetPaused(paused) => {
            core.set_property_string("pause", if paused { "yes" } else { "no" })
        }
        PlayerCommand::Seek { seconds, exact } => {
            let seconds = if seconds.is_finite() { seconds.max(0.0) } else { 0.0 };
            if assembler.status() == PlayerStatus::Loading {
                // Applied on MPV_EVENT_FILE_LOADED; seeking an unloaded file
                // would be dropped by mpv.
                assembler.set_pending_seek(seconds);
                return Ok(());
            }
            let value = format!("{seconds:.3}");
            let flags = if exact {
                "absolute+exact"
            } else {
                "absolute+keyframes"
            };
            core.command(&["seek", &value, flags])?;
            assembler.mark_dirty();
            Ok(())
        }
        PlayerCommand::SetVolume(volume) => {
            let clamped = if volume.is_finite() {
                volume.clamp(0.0, 100.0)
            } else {
                100.0
            };
            assembler.set_volume(clamped);
            core.set_property_string("volume", &format!("{clamped:.1}"))
        }
        PlayerCommand::SetMuted(muted) => {
            assembler.set_muted(muted);
            core.set_property_string("mute", if muted { "yes" } else { "no" })
        }
        PlayerCommand::SetTrack { kind, id } => {
            // id < 0 disables the track ("no" is mpv's sentinel).
            let value = if id < 0 {
                "no".to_string()
            } else {
                id.to_string()
            };
            core.set_property_string(kind.property(), &value)?;
            assembler.mark_dirty();
            Ok(())
        }
        PlayerCommand::Stop => {
            core.command(&["stop"])?;
            assembler.on_stopped();
            Ok(())
        }
        PlayerCommand::Shutdown | PlayerCommand::Snapshot(_) => Ok(()),
    }
}

fn handle_open(
    core: &MpvCore<'_>,
    assembler: &mut SnapshotAssembler,
    url: String,
    start_seconds: Option<f64>,
) -> Result<(), MpvError> {
    assembler.begin_open(start_seconds);
    // A previous `ended` (keep-open) leaves pause=true; clear it so the next
    // file actually starts.
    core.set_property_string("pause", "no")?;
    core.command(&["loadfile", &url])?;
    Ok(())
}

// ---- event handling -----------------------------------------------------------------

enum EventOutcome {
    Continue,
    Shutdown,
}

/// Copy of `mpv_event`'s scalar header plus its data pointer. The pointer is
/// only valid until the next `mpv_wait_event` call, so it is consumed
/// immediately.
struct EventFrame {
    id: MpvEventId,
    data: *mut c_void,
}

fn handle_event(
    core: &MpvCore<'_>,
    assembler: &mut SnapshotAssembler,
    frame: &EventFrame,
) -> EventOutcome {
    match frame.id {
        MPV_EVENT_SHUTDOWN => EventOutcome::Shutdown,
        MPV_EVENT_PROPERTY_CHANGE => {
            apply_property_change(core, assembler, frame.data);
            EventOutcome::Continue
        }
        MPV_EVENT_FILE_LOADED => {
            assembler.on_file_loaded();
            refresh_tracks(core, assembler);
            if let Some(seconds) = assembler.take_pending_seek() {
                if let Err(err) = core
                    .command(&["seek", &format!("{seconds:.3}"), "absolute+exact"])
                {
                    eprintln!("[emby-mpv] pending seek {seconds}s failed: {err}");
                }
            }
            EventOutcome::Continue
        }
        MPV_EVENT_END_FILE => {
            let mut reason = -1;
            let mut error = None;
            if !frame.data.is_null() {
                let end = unsafe { &*(frame.data as *const MpvEventEndFile) };
                reason = end.reason;
                if end.reason == MPV_END_FILE_REASON_ERROR {
                    // `mpv_error_string` is generic ("loading failed"), so add
                    // the operation context the UI should show.
                    error = Some(format!("playback failed: {}", core.lib.error_string(end.error)));
                }
            }
            assembler.on_end_file(reason, error);
            EventOutcome::Continue
        }
        MPV_EVENT_SEEK | MPV_EVENT_PLAYBACK_RESTART | MPV_EVENT_VIDEO_RECONFIG => {
            // A discontinuity happened: re-send the snapshot even if the
            // tracked properties did not change.
            assembler.mark_dirty();
            EventOutcome::Continue
        }
        MPV_EVENT_QUEUE_OVERFLOW => {
            // We lost events; resync the UI from the tracked state.
            assembler.mark_dirty();
            EventOutcome::Continue
        }
        _ => EventOutcome::Continue,
    }
}

fn apply_property_change(
    core: &MpvCore<'_>,
    assembler: &mut SnapshotAssembler,
    data: *mut c_void,
) {
    if data.is_null() {
        return;
    }
    let property = unsafe { &*(data as *const MpvEventProperty) };
    if property.name.is_null() {
        return;
    }
    let name = unsafe { CStr::from_ptr(property.name) }
        .to_str()
        .unwrap_or_default();

    match name {
        "pause" => {
            if let Some(value) = flag_value(property) {
                assembler.set_paused(value);
            }
        }
        "time-pos" => {
            if let Some(value) = double_value(property) {
                assembler.set_position(value);
            }
        }
        "duration" => {
            if let Some(value) = double_value(property) {
                assembler.set_duration(value);
            }
        }
        "volume" => {
            if let Some(value) = double_value(property) {
                assembler.set_volume(value);
            }
        }
        "mute" => {
            if let Some(value) = flag_value(property) {
                assembler.set_muted(value);
            }
        }
        "width" => {
            if let Some(value) = int64_value(property) {
                assembler.set_video_size(value, assembler.video_height());
            }
        }
        "height" => {
            if let Some(value) = int64_value(property) {
                assembler.set_video_size(assembler.video_width(), value);
            }
        }
        "eof-reached" => {
            if let Some(value) = flag_value(property) {
                assembler.set_eof_reached(value);
            }
        }
        "idle-active" => {
            if let Some(value) = flag_value(property) {
                assembler.set_idle_active(value);
            }
        }
        "paused-for-cache" => {
            if let Some(value) = flag_value(property) {
                assembler.set_buffering(value);
            }
        }
        "track-list" => refresh_tracks(core, assembler),
        _ => {}
    }
}

fn flag_value(property: &MpvEventProperty) -> Option<bool> {
    if property.format == MPV_FORMAT_FLAG && !property.data.is_null() {
        Some(unsafe { *(property.data as *const c_int) } != 0)
    } else {
        None
    }
}

fn double_value(property: &MpvEventProperty) -> Option<f64> {
    if property.format == MPV_FORMAT_DOUBLE && !property.data.is_null() {
        Some(unsafe { *(property.data as *const f64) })
    } else {
        None
    }
}

fn int64_value(property: &MpvEventProperty) -> Option<i64> {
    if property.format == MPV_FORMAT_INT64 && !property.data.is_null() {
        Some(unsafe { *(property.data as *const i64) })
    } else {
        None
    }
}

/// Re-read `track-list` on the mpv thread via `mpv_get_property_string`
/// (`track-list/N/...`), translating mpv's `"sub"` into the UI's `"subtitle"`.
fn refresh_tracks(core: &MpvCore<'_>, assembler: &mut SnapshotAssembler) {
    let count = core
        .get_property_string("track-list/count")
        .and_then(|value| value.trim().parse::<i64>().ok())
        .unwrap_or(0)
        .clamp(0, 64);

    let mut audio = Vec::new();
    let mut subtitles = Vec::new();
    for index in 0..count {
        let prefix = format!("track-list/{index}");
        let Some(kind) = core
            .get_property_string(&format!("{prefix}/type"))
            .as_deref()
            .and_then(TrackKind::from_mpv)
        else {
            continue;
        };
        let Some(id) = core
            .get_property_string(&format!("{prefix}/id"))
            .and_then(|value| value.trim().parse::<i64>().ok())
        else {
            continue;
        };
        if id < 0 {
            continue;
        }
        let title = core
            .get_property_string(&format!("{prefix}/title"))
            .unwrap_or_default();
        let lang = core
            .get_property_string(&format!("{prefix}/lang"))
            .map(|value| value.trim().to_string())
            .filter(|value| !value.is_empty());
        let selected = core
            .get_property_string(&format!("{prefix}/selected"))
            .map(|value| is_truthy(&value))
            .unwrap_or(false);

        let track = PlayerTrack::new(id, kind, title, lang, selected);
        match kind {
            TrackKind::Audio => audio.push(track),
            TrackKind::Subtitle => subtitles.push(track),
        }
    }
    assembler.set_tracks(audio, subtitles);
}

fn is_truthy(value: &str) -> bool {
    matches!(value.trim(), "yes" | "true" | "1")
}

// ---- the mpv handle owner -----------------------------------------------------------

/// Owns `mpv_handle`; `Drop` guarantees `mpv_terminate_destroy` runs exactly
/// once, on the mpv thread, after the event loop stopped.
struct MpvCore<'a> {
    lib: &'a MpvLibrary,
    handle: *mut MpvHandle,
    terminated: bool,
}

impl<'a> MpvCore<'a> {
    fn initialize(lib: &'a MpvLibrary, config: &SessionConfig) -> Result<Self, MpvError> {
        let handle = unsafe { (lib.api.create)() };
        if handle.is_null() {
            return Err(MpvError::NullHandle);
        }
        let core = Self {
            lib,
            handle,
            terminated: false,
        };

        // Options must be set before mpv_initialize(). `http-header-fields`
        // especially is rejected at runtime (and would leak into mpv.log).
        for option in build_initial_options(
            config.host_hwnd,
            config.log_path.as_deref(),
            &config.extra_options,
            config.headers.as_deref(),
        ) {
            let name = cstring("option name", &option.name)?;
            let value = cstring("option value", &option.value)?;
            let rc = unsafe { (lib.api.set_option_string)(core.handle, name.as_ptr(), value.as_ptr()) };
            if rc < 0 {
                let detail = core.lib.error_string(rc);
                if option.name == "wid" {
                    return Err(MpvError::from_code(
                        "mpv_set_option_string",
                        rc,
                        format!("could not embed into HWND {}: {detail}", option.value),
                    ));
                }
                // Unknown/renamed options on older mpv must not kill playback.
                eprintln!("[emby-mpv] option {} rejected: {detail}", option.name);
            }
        }

        let rc = unsafe { (lib.api.initialize)(core.handle) };
        if rc < 0 {
            return Err(core.failure("mpv_initialize", rc));
        }

        if let Some(request_log_messages) = lib.api.request_log_messages {
            if let Ok(level) = CString::new("warn") {
                let _ = unsafe { request_log_messages(core.handle, level.as_ptr()) };
            }
        }

        core.set_property_string("volume", &format!("{:.1}", config.volume.clamp(0.0, 100.0)))?;
        core.set_property_string("mute", if config.muted { "yes" } else { "no" })?;
        Ok(core)
    }

    fn observe_all(&self) {
        for (name, format) in OBSERVED {
            let Ok(cname) = CString::new(*name) else {
                continue;
            };
            let rc = unsafe { (self.lib.api.observe_property)(self.handle, 0, cname.as_ptr(), *format) };
            if rc < 0 {
                eprintln!(
                    "[emby-mpv] mpv_observe_property({name}) failed: {}",
                    self.lib.error_string(rc)
                );
            }
        }
    }

    fn command(&self, args: &[&str]) -> Result<(), MpvError> {
        let mut owned = Vec::with_capacity(args.len());
        for arg in args {
            owned.push(cstring("command argument", arg)?);
        }
        let mut pointers: Vec<*const c_char> = owned.iter().map(|arg| arg.as_ptr()).collect();
        pointers.push(std::ptr::null());
        let rc = unsafe { (self.lib.api.command)(self.handle, pointers.as_ptr()) };
        if rc < 0 {
            Err(self.failure("mpv_command", rc))
        } else {
            Ok(())
        }
    }

    fn set_property_string(&self, name: &str, value: &str) -> Result<(), MpvError> {
        let cname = cstring("property name", name)?;
        let cvalue = cstring("property value", value)?;
        let rc = unsafe { (self.lib.api.set_property_string)(self.handle, cname.as_ptr(), cvalue.as_ptr()) };
        if rc < 0 {
            Err(self.failure("mpv_set_property_string", rc))
        } else {
            Ok(())
        }
    }

    fn get_property_string(&self, name: &str) -> Option<String> {
        let cname = CString::new(name).ok()?;
        unsafe {
            let raw = (self.lib.api.get_property_string)(self.handle, cname.as_ptr());
            if raw.is_null() {
                return None;
            }
            let value = CStr::from_ptr(raw).to_string_lossy().into_owned();
            (self.lib.api.free)(raw as *mut c_void);
            Some(value)
        }
    }

    fn wait_event(&self, timeout: f64) -> Option<EventFrame> {
        let pointer = unsafe { (self.lib.api.wait_event)(self.handle, timeout) };
        if pointer.is_null() {
            return None;
        }
        let event = unsafe { &*pointer };
        if event.event_id == MPV_EVENT_NONE {
            return None;
        }
        Some(EventFrame {
            id: event.event_id,
            data: event.data,
        })
    }

    fn request_quit(&self) {
        if let Err(err) = self.command(&["quit"]) {
            eprintln!("[emby-mpv] quit command failed: {err}");
        }
    }

    fn failure(&self, operation: &'static str, code: i32) -> MpvError {
        MpvError::from_code(operation, code, self.lib.error_string(code))
    }

    fn terminate(&mut self) {
        if self.terminated || self.handle.is_null() {
            return;
        }
        self.terminated = true;
        unsafe { (self.lib.api.terminate_destroy)(self.handle) };
        self.handle = std::ptr::null_mut();
    }
}

impl Drop for MpvCore<'_> {
    fn drop(&mut self) {
        self.terminate();
    }
}

fn cstring(context: &'static str, value: &str) -> Result<CString, MpvError> {
    CString::new(value).map_err(|_| MpvError::InteriorNul { context })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn truthy_parsing_matches_mpv_flags() {
        assert!(is_truthy("yes"));
        assert!(is_truthy("true"));
        assert!(is_truthy("1"));
        assert!(is_truthy(" yes \n"));
        assert!(!is_truthy("no"));
        assert!(!is_truthy("0"));
        assert!(!is_truthy(""));
    }

    #[test]
    fn nul_arguments_are_rejected_before_ffi() {
        let err = cstring("command argument", "hello\0world").unwrap_err();
        assert!(matches!(err, MpvError::InteriorNul { .. }));
        assert!(cstring("command argument", "hello world").is_ok());
    }

    #[test]
    fn seek_flags_get_keyframe_or_exact_variants() {
        // Mirrors `handle_command`'s flag choice (mpv `seek` command syntax).
        fn flags(exact: bool) -> &'static str {
            if exact {
                "absolute+exact"
            } else {
                "absolute+keyframes"
            }
        }
        assert_eq!(flags(true), "absolute+exact");
        assert_eq!(flags(false), "absolute+keyframes");
    }

    #[test]
    fn shutdown_timeout_is_two_seconds() {
        assert_eq!(SHUTDOWN_TIMEOUT, Duration::from_secs(2));
        assert!(SNAPSHOT_INTERVAL <= Duration::from_millis(200));
        assert!(EVENT_TIMEOUT_SECS > 0.0 && EVENT_TIMEOUT_SECS <= 0.05);
    }
}
