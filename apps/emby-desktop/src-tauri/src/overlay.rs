//! Transparent controls surface owned by the main player window.
//!
//! The video is a native child `HWND` of the main window (see
//! `video_window.rs`), so it always paints above WebView2. CSS controls in the
//! main page would be covered by it; instead this module owns a separate
//! `controls` window that is positioned over the bottom strip of the video
//! rectangle reported by `video_sync`.
//!
//! The window is an owned window of the main window (`.parent(&main)`): Windows
//! keeps it above its owner, hides it when the owner is minimized and destroys
//! it with the owner, so no topmost flag or focus-driven show/hide is needed. It never takes focus (`focused(false)` + `focusable(false)`) and is
//! click-through while the controls are faded out (`overlay_set_interactive`).
//! Geometry is kept in physical pixels and re-applied on main-window move,
//! resize and DPI changes.
//!
//! Visibility is driven only by `player_ui_mode` and `video_sync.visible`. While
//! the video is visible the overlay is shown once and then only moved/resized;
//! it is never hidden on focus changes. This avoids the show/hide + ex-style
//! churn that made the bar flicker.
//!
//! Coordinate contract: `VideoSync.x/y/w/h` are CSS pixels relative to the
//! main webview client area. The client origin comes from `inner_position()`
//! (equal to `outer_position()` because the app window is always undecorated);
//! the clamp still uses the outer top so the overlay can never leave the window.
//!
//! Click routing: the surface is click-through while it is faded out, but the
//! page hides itself on a timer (`overlay_set_interactive(false)`). That call is
//! therefore deferred by [`CLICK_THROUGH_GRACE`]: a press that started while the
//! bar was still interactive must not be turned into `WS_EX_TRANSPARENT`
//! mid-gesture, otherwise Win32 never delivers the matching button-up and the
//! button's `click` is never synthesized. Turning interaction *on* is immediate
//! and cancels any pending click-through.

use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex, MutexGuard};
use std::time::{Duration, Instant};

use emby_mpv::{SeqLatch, VideoSync};
use tauri::window::Color;
use tauri::{
    AppHandle, Emitter, Manager, PhysicalPosition, State, WebviewUrl, WebviewWindow,
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

/// Fullscreen transition, step 1: really hide the owned overlay window.
/// A no-op when setup never created it.
pub fn suspend(app: &AppHandle) {
    if let Some(state) = app.try_state::<OverlayState>() {
        state.suspend(app);
    }
}

/// Fullscreen transition, step 2: re-present the overlay at the last known
/// rectangle. Called once the main window has its new client size.
pub fn resume(app: &AppHandle) {
    if let Some(state) = app.try_state::<OverlayState>() {
        state.resume(app);
    }
}

/// Logical (CSS px) heights of the parked control surface. Keep in sync with
/// `OVERLAY_BAR_HEIGHT` / `OVERLAY_EXPANDED_HEIGHT` (src/controls/contract.ts)
/// and `--ov-bar-h` / `--ov-panel-h` (src/controls/overlay.css).
const COLLAPSED_HEIGHT: f64 = 96.0;
const EXPANDED_HEIGHT: f64 = 352.0;

const WAKE_POLL_INTERVAL: Duration = Duration::from_millis(50);
/// Squared movement threshold in physical pixels (3 px).
const WAKE_MIN_MOVE_SQUARED: i64 = 9;
/// How long `overlay_set_interactive(false)` waits before the window really
/// becomes click-through. A single mouse gesture (down .. up .. click) is far
/// shorter; the delay only has to cover the gap between the page's auto-hide
/// timer and the button-up of a press that was already in flight.
const CLICK_THROUGH_GRACE: Duration = Duration::from_millis(450);
/// Hard cap for the "wait until the mouse button is released" extension in
/// `set_interactive(false)`. A stuck/dragged button must not pin the surface
/// interactive forever.
const CLICK_THROUGH_MAX_PRESS: Duration = Duration::from_millis(4000);

/// Last left-click handled by the stage poller, for double-click detection.
#[cfg(windows)]
static LAST_CLICK: Mutex<Option<(Instant, i32, i32)>> = Mutex::new(None);

#[derive(Clone, Copy, PartialEq, Eq)]
enum UiMode {
    App,
    Player,
}

struct Inner {
    mode: UiMode,
    expanded: bool,
    sync: Option<VideoSync>,
    /// Ignore out-of-order `video_sync` calls (page remounts send `seq == 0`).
    latch: SeqLatch,
    /// Last geometry pushed to the controls window, to avoid redundant calls.
    last_geometry: Option<(i32, i32, u32, u32)>,
    controls_shown: bool,
    /// Whether the controls should currently receive mouse input.
    interactive: bool,
    /// Last click-through state actually applied to the controls window.
    /// `None` means "unknown"; it is forced to the desired value after `show()`.
    cursor_ignored: Option<bool>,
    /// Bumped on every `interactive == true`; a deferred click-through whose
    /// generation is stale is dropped instead of swallowing a live press.
    interaction_generation: u64,
    /// Fullscreen transitions hide the surface and must not let a late
    /// `video_sync`/`present` re-show it in the middle of the resize.
    suspended: bool,
}

/// Managed state shared by the overlay commands, the window event handler and
/// the wake-up poller.
pub struct OverlayState {
    inner: Mutex<Inner>,
    wake_stop: Arc<AtomicBool>,
}

impl OverlayState {
    pub fn new() -> Self {
        Self {
            inner: Mutex::new(Inner {
                mode: UiMode::App,
                expanded: false,
                sync: None,
                latch: SeqLatch::new(),
                last_geometry: None,
                controls_shown: false,
                interactive: false,
                // `configure_controls_window` starts click-through before the
                // state is created, so no first `set_ignore_cursor_events`
                // round-trip is needed here.
                cursor_ignored: Some(true),
                interaction_generation: 0,
                suspended: false,
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

        // The main window keeps its frameless configuration in both modes, so
        // only the background and the overlay visibility change here. The mode
        // is set unconditionally to keep the call idempotent: repeating it
        // re-asserts the background and visibility after a remount.
        {
            let mut inner = self.lock();
            let next = if player { UiMode::Player } else { UiMode::App };
            let changed = inner.mode != next;
            inner.mode = next;
            // Leaving and re-entering the player is a new gesture budget.
            inner.suspended = false;
            if changed {
                eprintln!(
                    "[overlay] player_ui_mode -> {}",
                    if player { "player" } else { "app" }
                );
            }
        }

        if player {
            let mut first_error = None;
            if let Err(err) = main.set_background_color(Some(Color(0, 0, 0, 255))) {
                first_error.get_or_insert_with(|| format!("set background: {err}"));
            }
            if let Err(err) = self.present(app, None) {
                first_error.get_or_insert(err);
            }
            match first_error {
                Some(err) => Err(format!("player_ui_mode(\"player\"): {err}")),
                None => Ok(()),
            }
        } else {
            // Leaving the player: drop the overlay first, then restore the
            // normal app background. Decorations are never touched: the window
            // is frameless for the whole app lifetime.
            self.hide(app);
            main.set_background_color(None::<Color>)
                .map_err(|err| format!("player_ui_mode(\"app\"): reset background: {err}"))
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
        let generation = {
            let mut inner = self.lock();
            if inner.interactive != interactive {
                eprintln!("[overlay] interactive -> {interactive}");
            }
            inner.interactive = interactive;
            if interactive {
                // Turning input on must win over any deferred click-through.
                inner.interaction_generation = inner.interaction_generation.wrapping_add(1);
            }
            inner.interaction_generation
        };
        if interactive {
            if let Some(controls) = app.get_webview_window(CONTROLS_LABEL) {
                // `apply_click_through` compares against the state last applied
                // to the OS, so repeated commands with the same value do not
                // rewrite GWL_EXSTYLE (which forces a frame refresh and made
                // the bar flicker), while `overlay_set_interactive(false)`
                // still recovers if the window was shown in between.
                self.apply_click_through(&controls);
            }
            return;
        }
        // Click-through is deferred: the page calls this from its auto-hide
        // timer, which can fire between pointerdown and pointerup. Applying
        // `WS_EX_TRANSPARENT` there drops the button-up and the click never
        // happens (the reported "play/pause and fullscreen buttons do nothing"
        // bug). Nothing visual depends on this call, so waiting one gesture is
        // free; the wait is also extended while a mouse button is physically
        // down so a slow press cannot be cut in half either.
        let app = app.clone();
        std::thread::Builder::new()
            .name("overlay-click-through".to_string())
            .spawn(move || {
                std::thread::sleep(CLICK_THROUGH_GRACE);
                let deadline = Instant::now() + CLICK_THROUGH_MAX_PRESS;
                while pointer_button_down() && Instant::now() < deadline {
                    std::thread::sleep(Duration::from_millis(30));
                }
                let Some(state) = app.try_state::<OverlayState>() else {
                    return;
                };
                let still_wanted = {
                    let inner = state.lock();
                    !inner.interactive
                        && !inner.suspended
                        && inner.interaction_generation == generation
                };
                if !still_wanted {
                    return;
                }
                if let Some(controls) = app.get_webview_window(CONTROLS_LABEL) {
                    state.apply_click_through(&controls);
                }
            })
            .map(|_| ())
            .unwrap_or_else(|err| eprintln!("[overlay] click-through timer failed: {err}"));
    }

    /// Push the desired click-through state to the OS only when the applied
    /// state differs. `cursor_ignored == None` means "unknown" and is used
    /// right after `show()`, because showing can rewrite the ex-style.
    fn apply_click_through(&self, controls: &WebviewWindow) {
        let (interactive, applied) = {
            let inner = self.lock();
            (inner.interactive, inner.cursor_ignored)
        };
        let ignore = !interactive;
        if applied == Some(ignore) {
            return;
        }
        match controls.set_ignore_cursor_events(ignore) {
            Ok(()) => self.lock().cursor_ignored = Some(ignore),
            Err(err) => eprintln!("[overlay] could not set click-through to {ignore}: {err}"),
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

    /// Position the controls window, showing it once when the video becomes
    /// visible. Inside player mode this never hides/shows on focus changes:
    /// the overlay follows `video_sync.visible` and the player UI mode only.
    fn present(&self, app: &AppHandle, scale_override: Option<f64>) -> Result<(), String> {
        let (mode, sync, expanded, shown, suspended) = {
            let inner = self.lock();
            (
                inner.mode,
                inner.sync,
                inner.expanded,
                inner.controls_shown,
                inner.suspended,
            )
        };

        if mode != UiMode::Player || suspended {
            // A fullscreen transition hides the surface for its whole duration:
            // an owned window surviving a monitor/DWM change is one of the
            // triggers that makes Windows drop the borderless-fullscreen
            // optimization. Late `video_sync`/resize events must not re-show it.
            self.hide(app);
            return Ok(());
        }
        let Some(sync) = sync else {
            self.hide(app);
            return Ok(());
        };
        // `visible == false` means the page left the video (stop / unmount);
        // only then is the overlay hidden. A zero-size rect is treated the same
        // way because there is nothing sensible to position against.
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
        // Reserving the resize band only makes sense while tao actually offers
        // resize hit-testing (windowed, not maximized, not fullscreen).
        let resizable = !main.is_fullscreen().unwrap_or(false)
            && !main.is_maximized().unwrap_or(false);
        let frame = if resizable {
            resize_frame_px(scale)
        } else {
            (0, 0)
        };
        let geometry = compute_geometry(client_origin, outer_top, scale, &sync, expanded, frame);

        let geometry_changed = self.lock().last_geometry != Some(geometry);
        if geometry_changed {
            // One atomic SetWindowPos instead of separate size/position calls:
            // resizing a bottom-anchored transparent window in two steps makes
            // the bar jump (and flash) for a frame when a menu expands.
            apply_bounds(&controls, geometry)?;
            self.lock().last_geometry = Some(geometry);
        }

        if !shown {
            show_controls(&controls)?;
            self.lock().controls_shown = true;
            // `show()` can rewrite the ex-style from Tauri's window flags, so
            // the click-through state is re-applied once afterwards instead of
            // being repaired after every show/hide. `cursor_ignored` is marked
            // unknown first so the current state is really pushed.
            self.lock().cursor_ignored = None;
            self.apply_click_through(&controls);
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
            if let Err(err) = controls.hide() {
                eprintln!("[overlay] could not hide controls: {err}");
            } else {
                eprintln!("[overlay] controls hidden");
            }
            // The hidden window keeps its ex-style; mark the click-through
            // state unknown so the next show re-applies it.
            self.lock().cursor_ignored = None;
        }
    }

    /// Fullscreen transition: really hide the owned window (not just stop
    /// presenting it) and keep it hidden until [`Self::resume`].
    pub fn suspend(&self, app: &AppHandle) {
        let was_shown = self.lock().controls_shown;
        if !was_shown && self.lock().suspended {
            return;
        }
        self.lock().suspended = true;
        eprintln!("[overlay] suspend (fullscreen transition)");
        self.hide(app);
    }

    /// Leave the fullscreen transition and re-present at the last known rect.
    pub fn resume(&self, app: &AppHandle) {
        let was_suspended = {
            let mut inner = self.lock();
            let was = inner.suspended;
            inner.suspended = false;
            // The window was hidden with `hide()`, which already cleared
            // `cursor_ignored`; nothing else to reset.
            was
        };
        if !was_suspended {
            return;
        }
        eprintln!("[overlay] resume");
        if let Err(err) = self.present(app, None) {
            eprintln!("[overlay] could not re-present controls after fullscreen: {err}");
        }
    }

    /// Cursor-vs-video test for the wake poller. Returns `false` when the rect
    /// is unavailable so a transient failure cannot wake a hidden overlay.
    /// Focus is deliberately not consulted: the overlay is an owned window, so
    /// the OS hides it with its owner and focus changes must not drive it.
    #[cfg(windows)]
    fn cursor_inside_video(&self, main: &WebviewWindow, cursor: (i32, i32)) -> bool {
        let (mode, sync) = {
            let inner = self.lock();
            (inner.mode, inner.sync)
        };
        if mode != UiMode::Player {
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

    /// Screen-space test used by the click poller: true when the point is over
    /// the video and not over the (interactive) controls bar.
    #[cfg(windows)]
    fn is_stage_click(&self, app: &AppHandle, cursor: (i32, i32)) -> bool {
        let (mode, sync, shown, interactive, geometry) = {
            let inner = self.lock();
            (
                inner.mode,
                inner.sync,
                inner.controls_shown,
                inner.interactive,
                inner.last_geometry,
            )
        };
        if mode != UiMode::Player {
            return false;
        }
        let Some(sync) = sync else {
            return false;
        };
        if !sync.visible || sync.w <= 0.0 || sync.h <= 0.0 {
            return false;
        }
        let Some(main) = app.get_webview_window(MAIN_LABEL) else {
            return false;
        };
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
        if !(x >= left && x < right && y >= top && y < bottom) {
            return false;
        }
        // Clicks on a visible bar belong to the controls window, not the stage.
        if shown && interactive {
            if let Some((gx, gy, gw, gh)) = geometry {
                if cursor.0 >= gx
                    && cursor.0 < gx.saturating_add(gw as i32)
                    && cursor.1 >= gy
                    && cursor.1 < gy.saturating_add(gh as i32)
                {
                    return false;
                }
            }
        }
        // The pointer must actually be over our own window hierarchy: clicking
        // another app that happens to sit above the video rect must not steal
        // focus.
        let Ok(main_hwnd) = main.hwnd() else {
            return false;
        };
        let main_hwnd = main_hwnd.0 as isize as windows_sys::Win32::Foundation::HWND;
        let point = windows_sys::Win32::Foundation::POINT {
            x: cursor.0,
            y: cursor.1,
        };
        let under = unsafe { windows_sys::Win32::UI::WindowsAndMessaging::WindowFromPoint(point) };
        if under.is_null() {
            return false;
        }
        let ours = under == main_hwnd
            || unsafe { windows_sys::Win32::UI::WindowsAndMessaging::IsChild(main_hwnd, under) } != 0;
        ours
    }

    /// Focus the app (the native video window eats the click that would do it)
    /// and treat a second click inside the double-click window as "toggle
    /// fullscreen".
    #[cfg(windows)]
    fn on_stage_click(&self, app: &AppHandle, cursor: (i32, i32)) {
        if !self.is_stage_click(app, cursor) {
            return;
        }
        if let Some(main) = app.get_webview_window(MAIN_LABEL) {
            if let Ok(hwnd) = main.hwnd() {
                unsafe {
                    windows_sys::Win32::UI::WindowsAndMessaging::SetForegroundWindow(
                        hwnd.0 as isize as windows_sys::Win32::Foundation::HWND,
                    );
                }
            }
        }
        let _ = app.emit_to(CONTROLS_LABEL, "overlay-wake", ());
        let now = Instant::now();
        let mut last = LAST_CLICK
            .lock()
            .unwrap_or_else(|poison| poison.into_inner());
        let is_double = matches!(
            *last,
            Some((at, x, y))
                if now.duration_since(at) <= Duration::from_millis(unsafe {
                    windows_sys::Win32::UI::Input::KeyboardAndMouse::GetDoubleClickTime() as u64
                }) && (cursor.0 - x).abs() <= 4 && (cursor.1 - y).abs() <= 4
        );
        *last = if is_double {
            None
        } else {
            Some((now, cursor.0, cursor.1))
        };
        drop(last);
        if is_double {
            eprintln!("[overlay] stage double-click: toggle fullscreen");
            let _ = app.emit_to(MAIN_LABEL, "player-toggle-fullscreen", ());
        }
    }
}

fn compute_geometry(
    client_origin: PhysicalPosition<i32>,
    outer_top: i32,
    scale: f64,
    sync: &VideoSync,
    expanded: bool,
    frame: (i32, i32),
) -> (i32, i32, u32, u32) {
    let height_css = if expanded {
        EXPANDED_HEIGHT
    } else {
        COLLAPSED_HEIGHT
    };
    // The controls window must never cover the frameless main window resize
    // band: tao hit-tests SM_CXFRAME/SM_CYFRAME pixels *inside* the window rect,
    // and an owned window on top steals those hits. Leave that band free on the
    // left, right and bottom edge (the bar only touches those three).
    let (frame_x, frame_y) = frame;
    let x = client_origin.x + (sync.x * scale).round() as i32 + frame_x;
    let video_top = client_origin.y + (sync.y * scale).round() as i32;
    let width = ((sync.w * scale).round() as i32 - frame_x * 2).max(1) as u32;
    let video_bottom = video_top + (sync.h * scale).round().max(0.0) as i32;
    // Bottom-anchored: expanding grows upwards. Never above the main window:
    // on a short window the expanded surface shrinks instead of hanging below
    // the video (the popover scrolls).
    let room = (video_bottom - frame_y - outer_top).max(1);
    let height = ((height_css * scale).round() as i32).clamp(1, room) as u32;
    let y = (video_bottom - height as i32 - frame_y).max(outer_top);
    (x, y, width, height)
}

/// Physical thickness of the resize band tao hit-tests for an undecorated,
/// resizable window (it uses `SM_CXFRAME`/`SM_CYFRAME`, scaled by the monitor
/// DPI). Zero outside Windows.
#[cfg(windows)]
fn resize_frame_px(scale: f64) -> (i32, i32) {
    use windows_sys::Win32::UI::HiDpi::GetSystemMetricsForDpi;
    use windows_sys::Win32::UI::WindowsAndMessaging::{SM_CXFRAME, SM_CYFRAME};

    let dpi = (96.0 * scale).round().clamp(48.0, 480.0) as u32;
    unsafe {
        (
            GetSystemMetricsForDpi(SM_CXFRAME, dpi).max(0),
            GetSystemMetricsForDpi(SM_CYFRAME, dpi).max(0),
        )
    }
}

#[cfg(not(windows))]
fn resize_frame_px(_scale: f64) -> (i32, i32) {
    (0, 0)
}

/// Apply position and size in a single `SetWindowPos` so the bottom-anchored
/// overlay never paints at an intermediate rectangle while a menu expands.
fn apply_bounds(controls: &WebviewWindow, geometry: (i32, i32, u32, u32)) -> Result<(), String> {
    #[cfg(windows)]
    {
        use windows_sys::Win32::UI::WindowsAndMessaging::{
            SetWindowPos, SWP_NOACTIVATE, SWP_NOOWNERZORDER, SWP_NOZORDER,
        };

        let hwnd = controls_hwnd(controls)?;
        let ok = unsafe {
            SetWindowPos(
                hwnd,
                std::ptr::null_mut(),
                geometry.0,
                geometry.1,
                geometry.2 as i32,
                geometry.3 as i32,
                SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER,
            )
        };
        if ok == 0 {
            return Err("apply_bounds: SetWindowPos(controls) failed".to_string());
        }
        Ok(())
    }
    #[cfg(not(windows))]
    {
        controls
            .set_size(tauri::PhysicalSize::new(geometry.2, geometry.3))
            .map_err(|err| format!("set controls size: {err}"))?;
        controls
            .set_position(PhysicalPosition::new(geometry.0, geometry.1))
            .map_err(|err| format!("set controls position: {err}"))?;
        Ok(())
    }
}


// ---- window creation ---------------------------------------------------------

/// Create the hidden controls window during `setup`. A failure is logged by the
/// caller and must not prevent the library UI from running.
pub fn create_controls_window(app: &AppHandle) -> Result<WebviewWindow, String> {
    let main = app
        .get_webview_window(MAIN_LABEL)
        .ok_or_else(|| "controls window: missing main window".to_string())?;

    // `.parent(&main)` makes this an owned window on Windows: it always stays
    // above the owner in the z-order, is hidden when the owner is minimized and
    // is destroyed with it. That replaces the old topmost window flag and the
    // focus-driven show/hide dance.
    let window = WebviewWindowBuilder::new(
        app,
        CONTROLS_LABEL,
        WebviewUrl::App("controls.html".into()),
    )
    .parent(&main)
    .map_err(|err| format!("controls parent: {err}"))?
    .title("Emby Controls")
    .transparent(true)
    .decorations(false)
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

    // One-time style fix. `focusable(false)` already keeps the window
    // non-activating through Tauri, so only WS_EX_TOOLWINDOW (keep the helper
    // out of Alt+Tab) is set here. It is never re-applied afterwards: `set_ignore_cursor_events` and
    // `show` rebuild the ex-style from Tauri's flags and re-adding flags after
    // each call would force extra frame refreshes.
    #[cfg(windows)]
    apply_tool_window_style(window)?;
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
fn apply_tool_window_style(window: &WebviewWindow) -> Result<(), String> {
    use windows_sys::Win32::UI::WindowsAndMessaging::{
        GetWindowLongPtrW, SetWindowLongPtrW, GWL_EXSTYLE, WS_EX_TOOLWINDOW,
    };

    let hwnd = controls_hwnd(window)?;
    unsafe {
        let current = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        let wanted = current as u32 | WS_EX_TOOLWINDOW;
        if wanted as isize != current {
            SetWindowLongPtrW(hwnd, GWL_EXSTYLE, wanted as isize);
        }
        let check = GetWindowLongPtrW(hwnd, GWL_EXSTYLE) as u32;
        if check & WS_EX_TOOLWINDOW == 0 {
            return Err(format!(
                "controls ex-style is 0x{check:08x}; WS_EX_TOOLWINDOW missing"
            ));
        }
    }
    Ok(())
}

fn show_controls(window: &WebviewWindow) -> Result<(), String> {
    // Tauri keeps its own VISIBLE flag in sync; calling `ShowWindow` directly
    // would desynchronise it. Activation stays suppressed by focusable(false).
    window.show().map_err(|err| format!("show controls: {err}"))
}

/// True while any mouse button is physically down. Used only to delay the
/// click-through switch so a press in flight cannot be cancelled.
#[cfg(windows)]
fn pointer_button_down() -> bool {
    use windows_sys::Win32::UI::Input::KeyboardAndMouse::{
        GetAsyncKeyState, VK_LBUTTON, VK_MBUTTON, VK_RBUTTON,
    };

    let pressed = |vk: u16| unsafe { GetAsyncKeyState(vk as i32) } < 0;
    pressed(VK_LBUTTON) || pressed(VK_RBUTTON) || pressed(VK_MBUTTON)
}

#[cfg(not(windows))]
fn pointer_button_down() -> bool {
    false
}

/// Returns (is-down, was-pressed-since-last-poll) for the left mouse button.
#[cfg(windows)]
fn left_button_state() -> (bool, bool) {
    use windows_sys::Win32::UI::Input::KeyboardAndMouse::{GetAsyncKeyState, VK_LBUTTON};

    let state = unsafe { GetAsyncKeyState(VK_LBUTTON as i32) };
    (state < 0, (state as u16 & 0x0001) != 0)
}

// ---- main window chrome (Windows only) --------------------------------------

/// Remove the residual Win11 caption bits a frameless window keeps in its
/// style, without touching anything else.
///
/// Why this exists: `decorations: false` makes tao draw a zero-size non-client
/// area through `WM_NCCALCSIZE`, but it still creates the window with
/// `WS_CAPTION | WS_SYSMENU` (see tao's `WindowFlags::to_window_styles`) and
/// `WS_EX_WINDOWEDGE`. On Win11 that combination lets the DWM paint a 1px system
/// line along the top edge of the client area - visible exactly when the window
/// is flush with the top of the screen (dragged to the top / maximized), which
/// is the reported gap.
///
/// Deliberately minimal: `WS_THICKFRAME` is preserved when present (resize
/// borders, Aero Snap and the maximize insets depend on it) and never added, so
/// this is a no-op while tao has the borderless-fullscreen flag set and the
/// style legitimately has no frame bits. `WS_EX_WINDOWEDGE` (a 3D border that is
/// never drawn but still affects the frame calculation) is dropped.
///
/// Takes the raw `HWND` (as `isize`) instead of a Tauri window type: the
/// `on_window_event` callback hands out `&tauri::Window`, while `setup` only has
/// a `&WebviewWindow`, and both expose their own `hwnd()` behind `#[cfg(windows)]`.
/// Passing the handle keeps this callable from either place and from
/// non-Windows builds.
#[cfg(windows)]
pub fn harden_main_window_chrome(hwnd: isize) -> Result<(), String> {
    use windows_sys::Win32::UI::WindowsAndMessaging::{
        SetWindowLongPtrW, GWL_EXSTYLE, GWL_STYLE, SWP_FRAMECHANGED, SWP_NOACTIVATE,
        SWP_NOMOVE, SWP_NOSIZE, SWP_NOZORDER, WS_CAPTION, WS_DLGFRAME, WS_EX_WINDOWEDGE,
    };
    use windows_sys::Win32::UI::WindowsAndMessaging::{SetWindowPos, GetWindowLongPtrW};

    if hwnd == 0 {
        return Err("main window HWND is null".to_string());
    }
    let hwnd = hwnd as windows_sys::Win32::Foundation::HWND;
    unsafe {
        let style = GetWindowLongPtrW(hwnd, GWL_STYLE) as u32;
        let ex_style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE) as u32;
        // `WS_CAPTION` already covers `WS_BORDER | WS_DLGFRAME`; both are listed
        // so the intent survives a future constant rename.
        let wanted_style = style & !(WS_CAPTION | WS_DLGFRAME);
        let wanted_ex = ex_style & !WS_EX_WINDOWEDGE;
        if wanted_style == style && wanted_ex == ex_style {
            return Ok(());
        }
        if wanted_style != style {
            SetWindowLongPtrW(hwnd, GWL_STYLE, wanted_style as isize);
        }
        if wanted_ex != ex_style {
            SetWindowLongPtrW(hwnd, GWL_EXSTYLE, wanted_ex as isize);
        }
        // Recompute the non-client area so the change applies to the live window
        // instead of the next resize; NOSIZE/NOMOVE keeps this out of the resize
        // path and cannot recurse into `WindowEvent::Resized`.
        SetWindowPos(
            hwnd,
            std::ptr::null_mut(),
            0,
            0,
            0,
            0,
            SWP_FRAMECHANGED | SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER,
        );
    }
    Ok(())
}

#[cfg(not(windows))]
pub fn harden_main_window_chrome(_hwnd: isize) -> Result<(), String> {
    Ok(())
}

// ---- cursor wake poller ------------------------------------------------------

/// Poll the cursor every 50 ms while the app runs. The loop stays parked (no
/// Win32 calls) outside player UI mode; `stop_wake` is checked every tick and
/// the thread is detached so shutdown never waits on it. Besides waking the
/// bar on pointer movement, the same tick samples the left mouse button so a
/// click on the native video window (which swallows the real mouse event) still
/// focuses the app and can toggle fullscreen on a double-click.
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
            let mut last_left_down = false;
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
                    last_left_down = false;
                    continue;
                }
                let Some(main) = app.get_webview_window(MAIN_LABEL) else {
                    continue;
                };
                let Some(cursor) = cursor_position() else {
                    continue;
                };
                // The video is a native mpv child window, so it swallows the
                // click that should focus the app / toggle fullscreen. Poll the
                // real button state and implement both here.
                let (left_down, left_pressed) = left_button_state();
                if (left_down && !last_left_down) || left_pressed {
                    state.on_stage_click(&app, cursor);
                }
                last_left_down = left_down;
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
