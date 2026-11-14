//! Transparent controls surface owned by the main player window.
//!
//! The video is a native child `HWND` of the main window (see
//! `video_window.rs`), so it always paints above WebView2. CSS controls in the
//! main page would be covered by it; instead this module owns a separate
//! `controls` window that is positioned over the bottom strip of the video
//! rectangle reported by `video_sync`.
//!
//! The surface covers the whole video rectangle so the top-left return/logo
//! strip and the bottom transport bar can live in one window and share one
//! auto-hide group; the window region (see [`apply_region`]) exposes only those
//! two strips, so everything between them stays click-through video.
//!
//! The window is an owned window of the main window (`.parent(&main)`): Windows
//! keeps it above its owner, hides it when the owner is minimized and destroys
//! it with the owner, so no topmost flag or focus-driven show/hide is needed. It never takes focus (`focused(false)` + `focusable(false)`) and is
//! click-through while the controls are faded out (`overlay_set_interactive`).
//! Geometry is kept in physical pixels and re-applied on main-window move,
//! resize and DPI changes, which arrive as Tauri window events (see `main.rs`);
//! nothing here polls the window rectangle.
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
//! Menus never resize the window. It always has the expanded size, and a
//! window region exposes only the bottom bar while no menu is open (see
//! [`OverlayState::present`]). Resizing on every open/close made WebView2
//! present one stale frame with the bar at the old offset, which showed as a
//! flash of the bar whenever a track was picked.
//!
//! Click routing: the surface is click-through while it is faded out, but the
//! page hides itself on a timer (`overlay_set_interactive(false)`). That call is
//! a state change applied immediately, with one guard: while a mouse button is
//! physically down the switch waits for the release (bounded, see
//! [`CLICK_THROUGH_MAX_PRESS`]). A press that started while the bar was still
//! interactive must not be turned into `WS_EX_TRANSPARENT` mid-gesture,
//! otherwise Win32 never delivers the matching button-up and the button's
//! `click` is never synthesized. Turning interaction *on* is always immediate
//! and cancels any pending click-through.
//!
//! Timing model — there are no fixed sleeps left on the interaction path. Every
//! transition is caused by an event (a command from the page, a window
//! message, a button state), and the two remaining timers are bounded
//! fallbacks whose only job is to recover from a page that stopped talking:
//!
//! | transition | before | now |
//! |---|---|---|
//! | click-through on | always wait 450 ms, then poll up to 4 s | apply now; wait only while a button is down |
//! | first-show collapse | sleep 220 ms unconditionally | page's own state change settles it; 400 ms fallback |
//! | cursor wake | 50 ms fixed poll | 16 ms while over the video, 250 ms elsewhere |

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

/// Logical (CSS px) heights of the control surface: the window always has the
/// expanded height; the collapsed height is the strip the window region leaves
/// visible and clickable while no menu is open. Keep in sync with
/// `OVERLAY_BAR_HEIGHT` / `OVERLAY_EXPANDED_HEIGHT` (src/controls/contract.ts)
/// and `--ov-bar-h` / `--ov-panel-h` (src/controls/overlay.css).
const COLLAPSED_HEIGHT: f64 = 96.0;
const EXPANDED_HEIGHT: f64 = 352.0;
/// Logical (CSS px) size of the top-left return/logo strip. The region only
/// exposes this corner, not a full-width band, so clicks on the rest of the
/// video's top edge still reach the video.
const TOP_BAND_HEIGHT: f64 = 88.0;
const TOP_BAND_MAX_WIDTH: f64 = 620.0;

/// Cursor poll interval while the pointer is over the video.
///
/// A `WH_MOUSE_LL` hook would be the fully event-driven answer, but it needs a
/// dedicated message-pumping thread and, more importantly, it sits in the
/// system-wide input path: one slow callback there stalls the mouse for every
/// application on the machine. The poll below is two cheap reads
/// (`GetCursorPos` + `GetAsyncKeyState`) and is only ever active while the
/// overlay is in player mode, so it is kept — with the interval split by what
/// the tick is actually for (see the two constants below).
const WAKE_POLL_ACTIVE: Duration = Duration::from_millis(16);
/// Interval while the pointer is outside the video rect. Nothing can happen
/// there, so this only has to notice the pointer coming back: 4 Hz instead of
/// the old fixed 20 Hz.
const WAKE_POLL_IDLE: Duration = Duration::from_millis(250);
/// Squared movement threshold in physical pixels (3 px).
const WAKE_MIN_MOVE_SQUARED: i64 = 9;
/// Fallback bound for `overlay_set_interactive(false)` while a mouse button is
/// physically down. The normal path applies click-through immediately; this
/// only covers the case where the page's auto-hide timer fires between
/// `pointerdown` and `pointerup`, where switching to `WS_EX_TRANSPARENT`
/// mid-gesture would drop the button-up and the button's `click` would never be
/// synthesized (the reported "play/pause does nothing" bug). A stuck or dragged
/// button must not pin the surface interactive, hence the cap.
const CLICK_THROUGH_MAX_PRESS: Duration = Duration::from_millis(4000);
/// Fallback bound for the first-show warm-up. Normally the collapse is driven
/// by the page's first `overlay_set_interactive`/`overlay_set_expanded` after
/// the controls window is shown; this only fires when the page stays silent.
const WARMUP_FALLBACK: Duration = Duration::from_millis(400);

/// Last left-click handled by the stage poller, for double-click detection.
#[cfg(windows)]
static LAST_CLICK: Mutex<Option<(Instant, i32, i32)>> = Mutex::new(None);

#[derive(Clone, Copy, PartialEq, Eq)]
enum UiMode {
    App,
    Player,
}

/// Physical-pixel extents of the two strips the window region exposes: the
/// top-left return/logo corner and the bottom transport surface. An expanded
/// overlay grows only the bottom strip (the track popover sits above the bar).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct Bands {
    top_width: u32,
    top_height: u32,
    bottom_height: u32,
}

struct Inner {
    mode: UiMode,
    expanded: bool,
    sync: Option<VideoSync>,
    /// Ignore out-of-order `video_sync` calls (page remounts send `seq == 0`).
    latch: SeqLatch,
    /// Last geometry pushed to the controls window, to avoid redundant calls.
    last_geometry: Option<(i32, i32, u32, u32)>,
    /// Strips (physical px) the window region exposes; `None` until the first
    /// region is applied. The top strip is clipped to its own width so only the
    /// corner is hit-testable, the bottom strip spans the window.
    last_bands: Option<Bands>,
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
    /// Set by the first `present()` after showing: the window is temporarily
    /// at full height so the webview allocates the taller surface while the bar
    /// is still faded out. The next state change from the page (or
    /// `WARMUP_FALLBACK`) collapses it back to the bar strip.
    warmup_pending: bool,
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
                last_bands: None,
                controls_shown: false,
                interactive: false,
                // `configure_controls_window` starts click-through before the
                // state is created, so no first `set_ignore_cursor_events`
                // round-trip is needed here.
                cursor_ignored: Some(true),
                interaction_generation: 0,
                suspended: false,
                warmup_pending: false,
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
            // The page reporting its state is the "webview is up and drawn"
            // signal the first-show warm-up waits for; clearing it here means
            // the `present` below also clips the surface back to the bar.
            inner.warmup_pending = false;
            inner.mode == UiMode::Player
        };
        if player {
            if let Err(err) = self.present(app, None) {
                eprintln!("[overlay] could not resize controls: {err}");
            }
        }
    }

    pub fn set_interactive(&self, app: &AppHandle, interactive: bool) {
        let (generation, settled) = {
            let mut inner = self.lock();
            if inner.interactive != interactive {
                eprintln!("[overlay] interactive -> {interactive}");
            }
            inner.interactive = interactive;
            // Same settle signal as `set_expanded`: the page sends this from its
            // own show/hide effect, so it means "the webview is up and has
            // drawn". Clearing the flag is not enough on its own — the region
            // has to be clipped back to the bar, which `present` does.
            let settled = std::mem::take(&mut inner.warmup_pending);
            if interactive {
                // Turning input on must win over any deferred click-through.
                inner.interaction_generation = inner.interaction_generation.wrapping_add(1);
            }
            (inner.interaction_generation, settled)
        };
        if settled {
            // Collapse the first-show full-height surface to the bar strip.
            if let Err(err) = self.present(app, None) {
                eprintln!("[overlay] warm-up collapse failed: {err}");
            }
        }
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
        // Click-through is a *state*, not a delay: apply it now unless a mouse
        // button is physically down. The page calls this from its auto-hide
        // timer, which can fire between `pointerdown` and `pointerup`; switching
        // to `WS_EX_TRANSPARENT` inside that window drops the button-up and the
        // button click is never synthesized (the reported "play/pause and
        // fullscreen buttons do nothing" bug).
        //
        // The gesture is *observed*, not timed: the old code always slept 450 ms
        // first, so after every auto-hide the surface stayed clickable for
        // almost half a second and silently swallowed clicks meant for the
        // video, then waited up to 4 s more. The common case now costs nothing
        // and the button-down case lasts only as long as the gesture.
        if !pointer_button_down() {
            if let Some(controls) = app.get_webview_window(CONTROLS_LABEL) {
                self.apply_click_through(&controls);
            }
            return;
        }
        // Only reached while a button is held: wait for the release, bounded.
        let app = app.clone();
        std::thread::Builder::new()
            .name("overlay-click-through".to_string())
            .spawn(move || {
                let deadline = Instant::now() + CLICK_THROUGH_MAX_PRESS;
                while pointer_button_down() && Instant::now() < deadline {
                    std::thread::sleep(Duration::from_millis(16));
                }
                let Some(state) = app.try_state::<OverlayState>() else {
                    return;
                };
                // Re-check: the page may have turned input back on (a new
                // generation) while the button was down. That deferred apply is
                // then stale and must not swallow the next gesture.
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
            .unwrap_or_else(|err| eprintln!("[overlay] click-through wait failed: {err}"));
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
        // The window keeps the expanded size; opening or closing a menu only
        // changes the window region (the visible, hit-testable bottom strip),
        // so the webview is never resized and cannot show a stale frame.
        let geometry = compute_geometry(client_origin, outer_top, scale, &sync, frame);
        let bands = active_bands(geometry.2, geometry.3, scale, expanded);

        let (geometry_changed, region_changed) = {
            let inner = self.lock();
            let geometry_changed = inner.last_geometry != Some(geometry);
            (geometry_changed, geometry_changed || inner.last_bands != Some(bands))
        };
        if geometry_changed {
            // One atomic SetWindowPos instead of separate size/position calls.
            apply_bounds(&controls, geometry)?;
            self.lock().last_geometry = Some(geometry);
        }
        if region_changed {
            apply_region(&controls, geometry.2, geometry.3, bands)?;
            self.lock().last_bands = Some(bands);
        }

        if !shown {
            // First show in this playback: expose the full-height surface for
            // one moment while the bar is still faded out, so the first menu
            // does not pay for allocating the taller window surface (that used
            // to flash).
            //
            // The collapse is event driven: `warmup_pending` is cleared by the
            // next state change the page makes (`overlay_set_interactive` /
            // `overlay_set_expanded`), which always follows the show because the
            // page owns the auto-hide timer. The 220 ms sleep this replaces fired
            // whether or not the page had settled, so a bar that the user was
            // already interacting with could be clipped mid-gesture.
            let full = Bands {
                top_width: geometry.2,
                top_height: geometry.3,
                bottom_height: geometry.3,
            };
            apply_region(&controls, geometry.2, geometry.3, full)?;
            self.lock().last_bands = Some(full);
            show_controls(&controls)?;
            {
                let mut inner = self.lock();
                inner.controls_shown = true;
                inner.warmup_pending = true;
                // `show()` can rewrite the ex-style from Tauri's window flags,
                // so the click-through state is re-applied once afterwards
                // instead of being repaired after every show/hide.
                // `cursor_ignored` is marked unknown first so the current state
                // is really pushed.
                inner.cursor_ignored = None;
            }
            self.apply_click_through(&controls);
            // Fallback only, for a page that never reports its state back (a
            // dead webview): the surface must not stay full-height forever.
            let app = app.clone();
            std::thread::spawn(move || {
                std::thread::sleep(WARMUP_FALLBACK);
                let Some(state) = app.try_state::<OverlayState>() else {
                    return;
                };
                if !state.lock().warmup_pending {
                    return;
                }
                if let Err(err) = state.present(&app, None) {
                    eprintln!("[overlay] warm-up fallback failed: {err}");
                }
            });
        }
        Ok(())
    }

    fn hide(&self, app: &AppHandle) {
        let was_shown = {
            let mut inner = self.lock();
            let shown = inner.controls_shown;
            inner.controls_shown = false;
            // A hidden window cannot be warmed up; the next show arms it again.
            // Without this the fallback thread could re-present a window the
            // page already left.
            inner.warmup_pending = false;
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
        let (mode, sync, shown, interactive, geometry, bands) = {
            let inner = self.lock();
            (
                inner.mode,
                inner.sync,
                inner.controls_shown,
                inner.interactive,
                inner.last_geometry,
                inner.last_bands,
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
        // Clicks on a visible strip belong to the controls window, not the
        // stage. Only the two strips the window region exposes count: anywhere
        // else the window is clipped and the click reaches the video.
        if shown && interactive {
            if let (Some((gx, gy, gw, gh)), Some(bands)) = (geometry, bands) {
                let bottom = gy.saturating_add(gh as i32);
                let right = gx.saturating_add(gw as i32);
                let in_top = cursor.0 >= gx
                    && cursor.0 < gx.saturating_add(bands.top_width.min(gw) as i32)
                    && cursor.1 >= gy
                    && cursor.1 < gy.saturating_add(bands.top_height.min(gh) as i32);
                let bottom_top = bottom.saturating_sub(bands.bottom_height.min(gh) as i32);
                let in_bottom = cursor.0 >= gx
                    && cursor.0 < right
                    && cursor.1 >= bottom_top
                    && cursor.1 < bottom;
                if in_top || in_bottom {
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
    frame: (i32, i32),
) -> (i32, i32, u32, u32) {
    // The controls window must never cover the frameless main window resize
    // band: tao hit-tests SM_CXFRAME/SM_CYFRAME pixels *inside* the window rect,
    // and an owned window on top steals those hits. Leave that band free on
    // every edge the surface touches.
    let (frame_x, frame_y) = frame;
    let x = client_origin.x + (sync.x * scale).round() as i32 + frame_x;
    let video_top = client_origin.y + (sync.y * scale).round() as i32;
    let width = ((sync.w * scale).round() as i32 - frame_x * 2).max(1) as u32;
    let video_bottom = video_top + (sync.h * scale).round().max(0.0) as i32;
    // Cover the whole video rectangle: the top-left return/logo strip and the
    // bottom transport surface are two regions of the same window, so the
    // top-left corner finally has a window to draw in. Never above the main
    // window and never over its bottom resize band; on a short window the
    // surface shrinks instead of hanging outside the video.
    let y = (video_top + frame_y).max(outer_top);
    let bottom = (video_bottom - frame_y).max(y + 1);
    let height = (bottom - y).max(1) as u32;
    (x, y, width, height)
}

/// Extents of the two strips the window region exposes. The bottom strip is
/// the bar, or the bar plus the track-popover area while a menu is open; the
/// top strip is the return/logo corner. Never larger than the window.
fn active_bands(window_width: u32, window_height: u32, scale: f64, expanded: bool) -> Bands {
    let bottom = if expanded {
        (EXPANDED_HEIGHT * scale).round().max(1.0) as u32
    } else {
        (COLLAPSED_HEIGHT * scale).round().max(1.0) as u32
    }
    .min(window_height);
    let top_height = ((TOP_BAND_HEIGHT * scale).round().max(1.0) as u32).min(window_height);
    let top_width = ((TOP_BAND_MAX_WIDTH * scale).round().max(1.0) as u32).min(window_width);
    Bands {
        top_width,
        top_height,
        bottom_height: bottom,
    }
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

/// Clip the controls window to the top-left return/logo strip and the bottom
/// transport strip. Everything else is neither drawn nor hit-tested, so clicks
/// over the video reach the video even though the window now covers it. Unlike
/// a resize, changing the region leaves the webview's size and layout alone, so
/// no stale frame can flash.
fn apply_region(
    controls: &WebviewWindow,
    width: u32,
    height: u32,
    bands: Bands,
) -> Result<(), String> {
    #[cfg(windows)]
    {
        use windows_sys::Win32::Graphics::Gdi::{
            CombineRgn, CreateRectRgn, DeleteObject, SetWindowRgn, RGN_OR,
        };

        let hwnd = controls_hwnd(controls)?;
        // Always a concrete region, even for the whole window: clearing a
        // region makes Windows repaint the entire window, which showed up as a
        // one-frame flash the first time a menu opened.
        let top_width = bands.top_width.min(width) as i32;
        let top_height = bands.top_height.min(height) as i32;
        let bottom_top = height.saturating_sub(bands.bottom_height.min(height)) as i32;
        let region = unsafe { CreateRectRgn(0, 0, 0, 0) };
        if region.is_null() {
            return Err("apply_region: CreateRectRgn failed".to_string());
        }
        let top_rect = unsafe { CreateRectRgn(0, 0, top_width, top_height) };
        let bottom_rect =
            unsafe { CreateRectRgn(0, bottom_top, width as i32, height as i32) };
        if top_rect.is_null() || bottom_rect.is_null() {
            unsafe {
                DeleteObject(region as _);
                if !top_rect.is_null() {
                    DeleteObject(top_rect as _);
                }
                if !bottom_rect.is_null() {
                    DeleteObject(bottom_rect as _);
                }
            }
            return Err("apply_region: CreateRectRgn failed".to_string());
        }
        let combined = unsafe { CombineRgn(region, top_rect, bottom_rect, RGN_OR) };
        unsafe {
            DeleteObject(top_rect as _);
            DeleteObject(bottom_rect as _);
        }
        if combined == 0 {
            unsafe {
                DeleteObject(region as _);
            }
            return Err("apply_region: CombineRgn failed".to_string());
        }
        // `bRedraw = 0`: the region only clips the existing WebView2 surface;
        // forcing a repaint here is what flashed the bar.
        let ok = unsafe { SetWindowRgn(hwnd, region, 0) };
        if ok == 0 {
            unsafe {
                DeleteObject(region as _);
            }
            return Err("apply_region: SetWindowRgn(controls) failed".to_string());
        }
        Ok(())
    }
    #[cfg(not(windows))]
    {
        let _ = (controls, width, height, bands);
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
    .inner_size(1.0, EXPANDED_HEIGHT)
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
            // Two rates instead of one fixed 50 ms tick: the fast rate is only
            // used while the pointer is over the video, where a movement means
            // something. Everywhere else the thread sleeps 250 ms, so an idle
            // library screen costs four Win32 reads per second instead of twenty.
            let mut interval = WAKE_POLL_IDLE;
            while !stop.load(Ordering::SeqCst) {
                std::thread::sleep(interval);
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
                    interval = WAKE_POLL_IDLE;
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
                    interval = WAKE_POLL_IDLE;
                    continue;
                }
                interval = WAKE_POLL_ACTIVE;
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

#[cfg(test)]
mod tests {
    use super::*;

    fn video(x: f64, y: f64, w: f64, h: f64) -> VideoSync {
        VideoSync {
            seq: 1,
            visible: true,
            x,
            y,
            w,
            h,
            dpr: 1.0,
        }
    }

    #[test]
    fn geometry_covers_the_whole_video_rect() {
        let (x, y, width, height) = compute_geometry(
            PhysicalPosition::new(100, 50),
            50,
            1.0,
            &video(0.0, 40.0, 1280.0, 720.0),
            (0, 0),
        );
        assert_eq!((x, y), (100, 90));
        assert_eq!((width, height), (1280, 720));
    }

    #[test]
    fn geometry_leaves_the_resize_band_free() {
        let (x, y, width, height) = compute_geometry(
            PhysicalPosition::new(100, 50),
            50,
            1.0,
            &video(0.0, 0.0, 1280.0, 720.0),
            (4, 4),
        );
        assert_eq!((x, y), (104, 54));
        assert_eq!((width, height), (1272, 712));
    }

    #[test]
    fn bands_expose_only_the_corner_and_the_bar() {
        let collapsed = active_bands(1280, 720, 1.0, false);
        assert_eq!(collapsed.top_width, 620);
        assert_eq!(collapsed.top_height, 88);
        assert_eq!(collapsed.bottom_height, 96);

        let expanded = active_bands(1280, 720, 1.0, true);
        assert_eq!(expanded.top_height, 88);
        assert_eq!(expanded.bottom_height, 352);

        // A short window never exposes strips larger than the window itself.
        let tiny = active_bands(320, 100, 1.0, true);
        assert_eq!(tiny.top_width, 320);
        assert!(tiny.top_height <= 100);
        assert!(tiny.bottom_height <= 100);
    }
}
