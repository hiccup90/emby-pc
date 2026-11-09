//! Transparent, always-on-top controls surface for the native player.
//!
//! The video is a native child `HWND` of the main window (see
//! `video_window.rs`), so it always paints above WebView2. CSS controls in the
//! main page would be covered by it; instead this module owns a separate
//! `controls` window that is positioned over the bottom strip of the video
//! rectangle reported by `video_sync`.
//!
//! The window never takes focus (`focused(false)` + `WS_EX_NOACTIVATE`) and is
//! click-through while the controls are faded out (`overlay_set_interactive`).
//! Geometry is kept in physical pixels and re-applied on main-window move,
//! resize and DPI changes.
//!
//! Coordinate contract: `VideoSync.x/y/w/h` are CSS pixels relative to the
//! main webview client area. The client origin comes from `inner_position()`
//! (equal to `outer_position()` while the player window is undecorated); the
//! clamp still uses the outer top so the overlay can never leave the window.

use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex, MutexGuard};
use std::time::Duration;

use emby_mpv::{SeqLatch, VideoSync};
use tauri::window::Color;
use tauri::{
    AppHandle, Emitter, Manager, PhysicalPosition, PhysicalSize, State, WebviewUrl, WebviewWindow,
    WebviewWindowBuilder,
};

pub(crate) const CONTROLS_LABEL: &str = "controls";

/// Entry point for the `video_sync` command. Missing state (setup failed) is a
/// no-op: the video host already reports the real error to the page.
pub fn on_video_sync(app: &AppHandle, sync: VideoSync) {
    if let Some(state) = app.try_state::<OverlayState>() {
        state.on_video_sync(app, sync);
    }
}
const MAIN_LABEL: &str = "main";

/// Logical (CSS px) heights of the parked control surface.
const COLLAPSED_HEIGHT: f64 = 64.0;
const EXPANDED_HEIGHT: f64 = 320.0;

const WAKE_POLL_INTERVAL: Duration = Duration::from_millis(100);
/// Squared movement threshold in physical pixels (3 px).
const WAKE_MIN_MOVE_SQUARED: i64 = 9;

#[derive(Clone, Copy, PartialEq, Eq)]
enum UiMode {
    App,
    Player,
}

#[derive(Clone, Copy)]
struct RestoreState {
    decorations: bool,
    shadow: bool,
}

struct Inner {
    mode: UiMode,
    expanded: bool,
    sync: Option<VideoSync>,
    /// Ignore out-of-order `video_sync` calls (page remounts send `seq == 0`).
    latch: SeqLatch,
    restore: Option<RestoreState>,
    /// The main window currently owns keyboard focus (and is not minimized).
    main_focused: bool,
    /// Last geometry pushed to the controls window, to avoid redundant calls.
    last_geometry: Option<(i32, i32, u32, u32)>,
    controls_shown: bool,
}

/// Managed state shared by the overlay commands, the window event handler and
/// the wake-up poller.
pub struct OverlayState {
    inner: Mutex<Inner>,
    wake_stop: Arc<AtomicBool>,
}

impl OverlayState {
    pub fn new(main_focused: bool) -> Self {
        Self {
            inner: Mutex::new(Inner {
                mode: UiMode::App,
                expanded: false,
                sync: None,
                latch: SeqLatch::new(),
                restore: None,
                main_focused,
                last_geometry: None,
                controls_shown: false,
            }),
            wake_stop: Arc::new(AtomicBool::new(false)),
        }
    }

    fn lock(&self) -> MutexGuard<'_, Inner> {
        self.inner
            .lock()
            .unwrap_or_else(|poison| poison.into_inner())
    }

    /// Ask the cursor poller to exit. Threads are not joined: the app may be
    /// exiting and the poller only reads Win32 state, so a short-lived detached
    /// thread is preferable to blocking shutdown.
    pub fn stop_wake(&self) {
        self.wake_stop.store(true, Ordering::SeqCst);
    }

    fn is_player_mode(&self) -> bool {
        self.lock().mode == UiMode::Player
    }

    // ---- state transitions --------------------------------------------------

    pub fn set_mode(&self, app: &AppHandle, player: bool) -> Result<(), String> {
        let main = app
            .get_webview_window(MAIN_LABEL)
            .ok_or_else(|| "player_ui_mode: missing main window".to_string())?;

        // Read the frame state only on the first player entry. Getter before
        // taking the state lock: window getters round-trip through the event
        // loop, and a window event handler may be waiting for that same lock.
        // Keeping the first snapshot also survives a quick player -> app ->
        // player toggle, where the restore patch may not have been applied yet.
        let need_snapshot = {
            let inner = self.lock();
            player && inner.mode != UiMode::Player && inner.restore.is_none()
        };
        let current_decorations = if need_snapshot {
            main.is_decorated().unwrap_or(true)
        } else {
            true
        };
        let (changed, restore) = {
            let mut inner = self.lock();
            let target = if player { UiMode::Player } else { UiMode::App };
            if inner.mode == target {
                (false, inner.restore)
            } else {
                if player && inner.restore.is_none() {
                    // Shadow has no getter in Tauri; the app window is created
                    // with the default (shadow on), so that is the restore value.
                    inner.restore = Some(RestoreState {
                        decorations: current_decorations,
                        shadow: true,
                    });
                }
                inner.mode = target;
                (true, inner.restore)
            }
        };

        if !changed {
            // Idempotent: keep the current mode, but re-assert visibility so a
            // repeated `player` call recovers from a focus/minimize hide.
            if player {
                self.present(app, None)?;
            } else {
                self.hide(app);
            }
            return Ok(());
        }

        if player {
            let mut first_error = None;
            if let Err(err) = main.set_background_color(Some(Color(0, 0, 0, 255))) {
                first_error.get_or_insert_with(|| format!("set background: {err}"));
            }
            if let Err(err) = main.set_decorations(false) {
                first_error.get_or_insert_with(|| format!("set decorations: {err}"));
            }
            if let Err(err) = main.set_shadow(false) {
                first_error.get_or_insert_with(|| format!("set shadow: {err}"));
            }
            if let Err(err) = self.present(app, None) {
                first_error.get_or_insert(err);
            }
            match first_error {
                Some(err) => Err(format!("player_ui_mode(\"player\"): {err}")),
                None => Ok(()),
            }
        } else {
            // Hide before restoring the frame so the overlay never floats over
            // a decorated window for a frame.
            self.hide(app);
            let restore = restore.unwrap_or(RestoreState {
                decorations: true,
                shadow: true,
            });
            let mut first_error = None;
            if let Err(err) = main.set_decorations(restore.decorations) {
                first_error.get_or_insert_with(|| format!("set decorations: {err}"));
            }
            if let Err(err) = main.set_shadow(restore.shadow) {
                first_error.get_or_insert_with(|| format!("set shadow: {err}"));
            }
            if let Err(err) = main.set_background_color(None::<Color>) {
                first_error.get_or_insert_with(|| format!("reset background: {err}"));
            }
            match first_error {
                Some(err) => Err(format!("player_ui_mode(\"app\"): {err}")),
                None => Ok(()),
            }
        }
    }

    fn on_video_sync(&self, app: &AppHandle, sync: VideoSync) {
        let player = {
            let mut inner = self.lock();
            if sync.seq == 0 {
                // The page remounted and restarted its counter.
                inner.latch.reset();
            }
            if !inner.latch.accept(sync.seq) {
                return;
            }
            inner.sync = Some(sync);
            inner.mode == UiMode::Player
        };
        if player {
            if let Err(err) = self.present(app, None) {
                eprintln!("[overlay] could not position controls: {err}");
            }
        }
    }

    pub fn set_expanded(&self, app: &AppHandle, expanded: bool) {
        let player = {
            let mut inner = self.lock();
            inner.expanded = expanded;
            inner.mode == UiMode::Player
        };
        if player {
            if let Err(err) = self.present(app, None) {
                eprintln!("[overlay] could not resize controls: {err}");
            }
        }
    }

    pub fn set_interactive(&self, app: &AppHandle, interactive: bool) {
        let Some(controls) = app.get_webview_window(CONTROLS_LABEL) else {
            return;
        };
        if let Err(err) = controls.set_ignore_cursor_events(!interactive) {
            eprintln!("[overlay] could not change click-through state: {err}");
        }
        // `set_ignore_cursor_events` recomputes the whole ex-style from Tauri's
        // flags, which drops the manually added WS_EX_TOOLWINDOW. (WS_EX_NOACTIVATE
        // survives because the window was built with focusable(false).)
        schedule_ex_style_repair(app);
    }

    pub fn on_main_focus(&self, app: &AppHandle, focused: bool) {
        {
            let mut inner = self.lock();
            inner.main_focused = focused;
        }
        if !focused {
            self.hide(app);
            return;
        }
        // Tauri has no WindowEvent::Minimized; a minimized window always loses
        // focus, so the restore path has to check IsIconic here.
        let minimized = app
            .get_webview_window(MAIN_LABEL)
            .and_then(|window| window.is_minimized().ok())
            .unwrap_or(false);
        if minimized {
            self.lock().main_focused = false;
            self.hide(app);
            return;
        }
        if let Err(err) = self.present(app, None) {
            eprintln!("[overlay] could not show controls after focus: {err}");
        }
    }

    pub fn on_main_geometry(&self, app: &AppHandle, scale_override: Option<f64>) {
        if !self.is_player_mode() {
            return;
        }
        if let Err(err) = self.present(app, scale_override) {
            eprintln!("[overlay] could not follow main window: {err}");
        }
    }

    pub fn on_main_destroyed(&self) {
        self.stop_wake();
    }

    // ---- geometry / visibility ---------------------------------------------

    /// Position and (if appropriate) show the controls window.
    fn present(&self, app: &AppHandle, scale_override: Option<f64>) -> Result<(), String> {
        let (mode, sync, expanded, main_focused) = {
            let inner = self.lock();
            (
                inner.mode,
                inner.sync,
                inner.expanded,
                inner.main_focused,
            )
        };

        if mode != UiMode::Player || !main_focused {
            self.hide(app);
            return Ok(());
        }
        let Some(sync) = sync else {
            self.hide(app);
            return Ok(());
        };
        if !sync.visible || sync.w <= 0.0 || sync.h <= 0.0 {
            self.hide(app);
            return Ok(());
        }

        let Some(main) = app.get_webview_window(MAIN_LABEL) else {
            self.hide(app);
            return Ok(());
        };
        let Some(controls) = app.get_webview_window(CONTROLS_LABEL) else {
            // Creation failed during setup; the library UI still works.
            return Ok(());
        };

        let scale = match scale_override {
            Some(scale) if scale.is_finite() && scale > 0.0 => scale,
            _ => main
                .scale_factor()
                .map_err(|err| format!("scale factor: {err}"))?,
        };
        let client_origin = main
            .inner_position()
            .map_err(|err| format!("inner position: {err}"))?;
        let outer_top = main
            .outer_position()
            .map_err(|err| format!("outer position: {err}"))?
            .y;
        let geometry = compute_geometry(client_origin, outer_top, scale, &sync, expanded);

        let geometry_changed = self.lock().last_geometry != Some(geometry);
        if geometry_changed {
            // Size first, then position: both are queued on the main thread and
            // the window stays hidden until the show below is processed.
            controls
                .set_size(PhysicalSize::new(geometry.2, geometry.3))
                .map_err(|err| format!("set controls size: {err}"))?;
            controls
                .set_position(PhysicalPosition::new(geometry.0, geometry.1))
                .map_err(|err| format!("set controls position: {err}"))?;
            self.lock().last_geometry = Some(geometry);
        }

        if !self.lock().controls_shown {
            // Queued after the geometry messages. `show()` is safe here: the
            // window is focusable(false) + WS_EX_NOACTIVATE, so showing it does
            // not take keyboard focus (and we never call `set_focus`).
            match show_controls(app, &controls) {
                Ok(()) => {
                    self.lock().controls_shown = true;
                    // Re-check: a focus loss / app-mode switch may have raced
                    // the show above and queued a hide behind it.
                    let still_wanted = {
                        let inner = self.lock();
                        inner.mode == UiMode::Player && inner.main_focused
                    };
                    if !still_wanted {
                        self.hide(app);
                    }
                }
                Err(err) => return Err(err),
            }
        }
        Ok(())
    }

    fn hide(&self, app: &AppHandle) {
        let was_shown = {
            let mut inner = self.lock();
            let shown = inner.controls_shown;
            inner.controls_shown = false;
            shown
        };
        if !was_shown {
            return;
        }
        if let Some(controls) = app.get_webview_window(CONTROLS_LABEL) {
            if let Err(err) = hide_controls(app, &controls) {
                eprintln!("[overlay] could not hide controls: {err}");
            }
        }
    }

    /// Cursor-vs-video test for the wake poller. Returns `false` when the rect
    /// is unavailable so a transient failure cannot wake a hidden overlay.
    #[cfg(windows)]
    fn cursor_inside_video(&self, main: &WebviewWindow, cursor: (i32, i32)) -> bool {
        let (mode, sync, main_focused) = {
            let inner = self.lock();
            (inner.mode, inner.sync, inner.main_focused)
        };
        if mode != UiMode::Player || !main_focused {
            return false;
        }
        let Some(sync) = sync else {
            return false;
        };
        if !sync.visible || sync.w <= 0.0 || sync.h <= 0.0 {
            return false;
        }
        let Ok(scale) = main.scale_factor() else {
            return false;
        };
        let Ok(origin) = main.inner_position() else {
            return false;
        };
        let left = origin.x as f64 + sync.x * scale;
        let top = origin.y as f64 + sync.y * scale;
        let right = left + sync.w * scale;
        let bottom = top + sync.h * scale;
        let x = cursor.0 as f64;
        let y = cursor.1 as f64;
        x >= left && x < right && y >= top && y < bottom
    }
}

fn compute_geometry(
    client_origin: PhysicalPosition<i32>,
    outer_top: i32,
    scale: f64,
    sync: &VideoSync,
    expanded: bool,
) -> (i32, i32, u32, u32) {
    let height_css = if expanded {
        EXPANDED_HEIGHT
    } else {
        COLLAPSED_HEIGHT
    };
    let x = client_origin.x + (sync.x * scale).round() as i32;
    let video_top = client_origin.y + (sync.y * scale).round() as i32;
    let width = (sync.w * scale).round().max(1.0) as u32;
    let height = (height_css * scale).round().max(1.0) as u32;
    let video_bottom = video_top + (sync.h * scale).round().max(0.0) as i32;
    // Bottom-anchored: expanding grows upwards. Never above the main window.
    let y = (video_bottom - height as i32).max(outer_top);
    (x, y, width, height)
}

// ---- window creation ---------------------------------------------------------

/// Create the hidden controls window during `setup`. A failure is logged by the
/// caller and must not prevent the library UI from running.
pub fn create_controls_window(app: &AppHandle) -> Result<WebviewWindow, String> {
    let window = WebviewWindowBuilder::new(
        app,
        CONTROLS_LABEL,
        WebviewUrl::App("controls.html".into()),
    )
    .title("Emby Controls")
    .transparent(true)
    .decorations(false)
    .always_on_top(true)
    .skip_taskbar(true)
    .shadow(false)
    .resizable(false)
    .focused(false)
    .focusable(false)
    .visible(false)
    .background_color(Color(0, 0, 0, 0))
    .inner_size(1.0, COLLAPSED_HEIGHT)
    .build()
    .map_err(|err| format!("controls window: {err}"))?;

    if let Err(err) = configure_controls_window(&window) {
        let _ = window.destroy();
        return Err(err);
    }
    Ok(window)
}

fn configure_controls_window(window: &WebviewWindow) -> Result<(), String> {
    // Invisible until a video rect is known; do not intercept the mouse while
    // parked.
    window
        .set_ignore_cursor_events(true)
        .map_err(|err| format!("controls click-through: {err}"))?;

    #[cfg(windows)]
    apply_no_activate_style(window)?;
    Ok(())
}

// ---- Win32 helpers (Windows only) -------------------------------------------

#[cfg(windows)]
fn controls_hwnd(window: &WebviewWindow) -> Result<windows_sys::Win32::Foundation::HWND, String> {
    // Tauri returns `windows::Win32::Foundation::HWND` (a newtype over
    // `*mut c_void`); windows-sys uses the bare pointer alias.
    let hwnd = window
        .hwnd()
        .map_err(|err| format!("controls HWND: {err}"))?
        .0 as isize;
    Ok(hwnd as windows_sys::Win32::Foundation::HWND)
}

#[cfg(windows)]
fn apply_no_activate_style(window: &WebviewWindow) -> Result<(), String> {
    use windows_sys::Win32::UI::WindowsAndMessaging::{
        GetWindowLongPtrW, SetWindowLongPtrW, GWL_EXSTYLE, WS_EX_NOACTIVATE, WS_EX_TOOLWINDOW,
    };

    let hwnd = controls_hwnd(window)?;
    unsafe {
        let current = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        let wanted = current as u32 | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;
        if wanted as isize != current {
            SetWindowLongPtrW(hwnd, GWL_EXSTYLE, wanted as isize);
        }
        let check = GetWindowLongPtrW(hwnd, GWL_EXSTYLE) as u32;
        let required = WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;
        if check & required != required {
            return Err(format!(
                "controls ex-style is 0x{check:08x}; WS_EX_NOACTIVATE|WS_EX_TOOLWINDOW missing"
            ));
        }
    }
    Ok(())
}

fn show_controls(app: &AppHandle, window: &WebviewWindow) -> Result<(), String> {
    // Tauri keeps its own VISIBLE flag in sync; calling `ShowWindow` directly
    // would desynchronise it and a later `set_ignore_cursor_events` would hide
    // the window again. Activation stays suppressed by WS_EX_NOACTIVATE.
    window.show().map_err(|err| format!("show controls: {err}"))?;
    // `show` rewrites the ex-style from Tauri's flags, dropping TOOLWINDOW.
    schedule_ex_style_repair(app);
    Ok(())
}

fn hide_controls(app: &AppHandle, window: &WebviewWindow) -> Result<(), String> {
    window.hide().map_err(|err| format!("hide controls: {err}"))?;
    schedule_ex_style_repair(app);
    Ok(())
}

/// Queue the ex-style repair behind the preceding Tauri window message. Both
/// use the same event-loop queue, so this also works when the command ran on an
/// IPC thread.
#[cfg(windows)]
fn schedule_ex_style_repair(app: &AppHandle) {
    let handle = app.clone();
    let task_handle = handle.clone();
    if let Err(err) = handle.run_on_main_thread(move || {
        if let Some(controls) = task_handle.get_webview_window(CONTROLS_LABEL) {
            if let Err(err) = apply_no_activate_style(&controls) {
                eprintln!("[overlay] could not re-apply controls ex-style: {err}");
            }
        }
    }) {
        eprintln!("[overlay] could not queue controls ex-style repair: {err}");
    }
}

#[cfg(not(windows))]
fn schedule_ex_style_repair(_app: &AppHandle) {}

// ---- cursor wake poller ------------------------------------------------------

/// Poll the cursor every 100 ms while the app runs. The loop stays parked (no
/// Win32 calls) outside player UI mode; `stop_wake` is checked every tick and
/// the thread is detached so shutdown never waits on it.
#[cfg(windows)]
pub fn spawn_wake_poller(app: AppHandle) {
    let stop = match app.try_state::<OverlayState>() {
        Some(state) => Arc::clone(&state.wake_stop),
        None => return,
    };
    let spawned = std::thread::Builder::new()
        .name("overlay-wake".to_string())
        .spawn(move || {
            let mut last: Option<(i32, i32)> = None;
            while !stop.load(Ordering::SeqCst) {
                std::thread::sleep(WAKE_POLL_INTERVAL);
                if stop.load(Ordering::SeqCst) {
                    break;
                }

                let Some(state) = app.try_state::<OverlayState>() else {
                    break;
                };
                if !state.is_player_mode() {
                    // Stop tracking so re-entering player mode starts fresh.
                    last = None;
                    continue;
                }
                let Some(main) = app.get_webview_window(MAIN_LABEL) else {
                    continue;
                };
                let Some(cursor) = cursor_position() else {
                    continue;
                };
                let inside = state.cursor_inside_video(&main, cursor);
                if !inside {
                    // Re-entering the video starts a fresh movement baseline.
                    last = None;
                    continue;
                }
                // Compare against the last wake, not the previous tick: slow
                // movement then still crosses the 3 px threshold eventually.
                match last {
                    Some((x, y)) => {
                        let dx = (cursor.0 - x) as i64;
                        let dy = (cursor.1 - y) as i64;
                        if dx * dx + dy * dy >= WAKE_MIN_MOVE_SQUARED {
                            // The controls page owns the show/hide timer; Rust
                            // only signals that the pointer moved over the video.
                            let _ = app.emit_to(CONTROLS_LABEL, "overlay-wake", ());
                            last = Some(cursor);
                        }
                    }
                    None => last = Some(cursor),
                }
            }
        });

    if let Err(err) = spawned {
        eprintln!("[overlay] could not start cursor poller: {err}");
    }
}

#[cfg(not(windows))]
pub fn spawn_wake_poller(_app: AppHandle) {}

#[cfg(windows)]
fn cursor_position() -> Option<(i32, i32)> {
    use windows_sys::Win32::Foundation::POINT;
    use windows_sys::Win32::UI::WindowsAndMessaging::GetCursorPos;

    let mut point = POINT::default();
    let ok = unsafe { GetCursorPos(&mut point) };
    (ok != 0).then_some((point.x, point.y))
}

// ---- Tauri commands ----------------------------------------------------------

#[tauri::command]
pub fn player_ui_mode(
    app: AppHandle,
    mode: String,
    state: State<'_, OverlayState>,
) -> Result<(), String> {
    let player = match mode.as_str() {
        "player" => true,
        "app" => false,
        other => {
            return Err(format!(
                "player_ui_mode: unknown mode {other:?} (expected \"player\" or \"app\")"
            ));
        }
    };
    state.set_mode(&app, player)
}

#[tauri::command]
pub fn overlay_set_expanded(
    app: AppHandle,
    expanded: bool,
    state: State<'_, OverlayState>,
) -> Result<(), String> {
    state.set_expanded(&app, expanded);
    Ok(())
}

#[tauri::command]
pub fn overlay_set_interactive(
    app: AppHandle,
    interactive: bool,
    state: State<'_, OverlayState>,
) -> Result<(), String> {
    state.set_interactive(&app, interactive);
    Ok(())
}

#[tauri::command]
pub fn overlay_drag_window(app: AppHandle) -> Result<(), String> {
    let main = app
        .get_webview_window(MAIN_LABEL)
        .ok_or_else(|| "overlay_drag_window: missing main window".to_string())?;
    main.start_dragging()
        .map_err(|err| format!("overlay_drag_window: {err}"))
}
