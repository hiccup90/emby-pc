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
//! churn that made the bar flicker. Pointer-move wakes, however, are gated on
//! the app being foreground: a player that is merely visible behind another
//! window must not pop its controls when the pointer crosses the video rect.
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
#[cfg(windows)]
use std::sync::atomic::AtomicU32;
use std::sync::{Arc, Mutex, MutexGuard};
use std::time::{Duration, Instant};

use emby_mpv::{SeqLatch, VideoSync};
use tauri::window::Color;
use tauri::{
    AppHandle, Manager, PhysicalPosition, State, WebviewUrl, WebviewWindow, WebviewWindowBuilder,
};
#[cfg(windows)]
use tauri::Emitter;

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

/// Logical pixels the controls window is lifted above the video rectangle when
/// `EMBY_TOP_MARGIN=1` is set.
///
/// Windows paints a window's non-client area at the very top of the window, and
/// the white strip is exactly that. The window *region* decides which window
/// pixels exist at all: everything outside it is neither painted nor hit-tested.
/// Lifting the window by this margin and starting the region below it puts the
/// window's top edge outside the window's own shape, which is a structural fix
/// instead of a style/repaint fight.
///
/// 40 logical px covers the 45-physical-pixel caption measured on a 200% display.
/// This layout is the default; `EMBY_TOP_MARGIN=0` is the documented way back.
const NC_SAFE_MARGIN: f64 = 40.0;

/// Diagnostic line that exists on every platform; the Windows-only `diag`
/// module is what actually persists it there.
fn log_diag(line: &str) {
    #[cfg(windows)]
    diag::write(line);
    #[cfg(not(windows))]
    let _ = line;
}

/// The top-margin layout is the default. `EMBY_TOP_MARGIN=0` (also `false`/`off`)
/// is the escape hatch back to the old geometry: `overlay_margin` then returns
/// zero, every geometry/region/hit-test formula reduces to the previous
/// behaviour, and the page gets no padding.
///
/// On real hardware the margin layout was verified end to end: no strip on
/// repeated pauses, strips in the right place, no stalls.
fn top_margin_enabled() -> bool {
    static ENABLED: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    *ENABLED.get_or_init(|| crate::env_flag("EMBY_TOP_MARGIN").unwrap_or(true))
}

/// The top margin in physical pixels; zero only when `EMBY_TOP_MARGIN=0`.
fn overlay_margin(scale: f64) -> i32 {
    if !top_margin_enabled() {
        return 0;
    }
    (NC_SAFE_MARGIN * scale).round().max(1.0) as i32
}

/// The top margin in logical (CSS) pixels for the controls page padding.
fn overlay_margin_css() -> i32 {
    if top_margin_enabled() {
        NC_SAFE_MARGIN as i32
    } else {
        0
    }
}

/// The two rectangles the window region exposes, in window-relative physical
/// pixels: the top-left corner strip and the bottom bar.
///
/// Single source of truth: `apply_region` builds the region from these and
/// `is_stage_click` maps them to screen space for hit-testing, so clipping and
/// click routing cannot drift apart. With `margin == 0` this is exactly the
/// pre-margin region: the top strip starts at the window's top edge.
#[cfg_attr(not(any(windows, test)), allow(dead_code))] // only the windows/test paths call it
fn region_rects(width: u32, height: u32, bands: Bands, margin: i32) -> [(i32, i32, i32, i32); 2] {
    let width = width as i32;
    let height = height as i32;
    let margin = margin.max(0).min(height.saturating_sub(1));
    let top_width = bands.top_width.min(width as u32) as i32;
    let top_bottom = (margin + bands.top_height.min(height as u32) as i32).min(height);
    let bottom_top = (height - bands.bottom_height.min(height as u32) as i32).max(margin);
    [
        (0, margin, top_width, top_bottom),
        (0, bottom_top, width, height),
    ]
}

/// Half-open screen-space hit test: left/top are inside, right/bottom are not.
#[cfg(any(windows, test))]
fn point_in_rect(
    cursor: (i32, i32),
    (x, y, width, height): (i32, i32, u32, u32),
) -> bool {
    let right = x.saturating_add(width.min(i32::MAX as u32) as i32);
    let bottom = y.saturating_add(height.min(i32::MAX as u32) as i32);
    cursor.0 >= x && cursor.0 < right && cursor.1 >= y && cursor.1 < bottom
}

/// Hysteresis for window repositioning, in physical pixels.
///
/// A window the compositor keeps nudging (or a geometry the page reports with a
/// fraction of a pixel of jitter) must not turn into a `SetWindowPos` per event:
/// with the margin layout that would be a visible feedback loop. One pixel of
/// slack is invisible and kills the oscillation.
const GEOMETRY_SLACK_PX: i32 = 1;

/// Whether the controls window actually has to move to reach `target`.
///
/// Returns `true` for a first placement and for any axis that differs by more
/// than [`GEOMETRY_SLACK_PX`]. Size and position are checked separately so a
/// one-pixel wobble in either one cannot keep a sixty-hertz present loop alive.
fn geometry_needs_move(
    current: Option<(i32, i32, u32, u32)>,
    target: (i32, i32, u32, u32),
) -> bool {
    let Some((cx, cy, cw, ch)) = current else {
        return true;
    };
    let (x, y, w, h) = target;
    (x - cx).abs() > GEOMETRY_SLACK_PX
        || (y - cy).abs() > GEOMETRY_SLACK_PX
        || (w as i32 - cw as i32).abs() > GEOMETRY_SLACK_PX
        || (h as i32 - ch as i32).abs() > GEOMETRY_SLACK_PX
}

/// Relocation budget: at most [`MAX_MOVES_PER_SECOND`] window moves per second.
///
/// Only used when the margin layout is on. One move per frame lets the overlay
/// follow a normal drag; a runaway event source is still capped and the skipped
/// move is logged instead of spinning the UI thread.
const MAX_MOVES_PER_SECOND: u32 = 60;

#[derive(Debug, Default)]
struct MoveBudget {
    window_start: Option<Instant>,
    count: u32,
}

impl MoveBudget {
    /// `true` when the move is allowed; the caller records it with
    /// [`MoveBudget::record`].
    fn allows(&mut self, now: Instant) -> bool {
        match self.window_start {
            Some(start) if now.duration_since(start) < Duration::from_secs(1) => {
                self.count < MAX_MOVES_PER_SECOND
            }
            _ => {
                self.window_start = Some(now);
                self.count = 0;
                true
            }
        }
    }

    fn record(&mut self) {
        self.count = self.count.saturating_add(1);
    }
}

/// `OVERLAY_BAR_HEIGHT` / `OVERLAY_EXPANDED_HEIGHT` (src/controls/contract.ts)
/// and `--ov-bar-h` / `--ov-panel-h` (src/controls/overlay.css).
const COLLAPSED_HEIGHT: f64 = 96.0;
const EXPANDED_HEIGHT: f64 = 352.0;

/// Commit the binary was built from, injected by `build.rs`.
fn build_id() -> &'static str {
    option_env!("EMBY_BUILD_SHA").unwrap_or("unknown")
}
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
#[cfg(windows)]
const WAKE_POLL_ACTIVE: Duration = Duration::from_millis(16);
/// Interval while the pointer is outside the video rect. Nothing can happen
/// there, so this only has to notice the pointer coming back: 4 Hz instead of
/// the old fixed 20 Hz.
#[cfg(windows)]
const WAKE_POLL_IDLE: Duration = Duration::from_millis(250);
/// Squared movement threshold in physical pixels (3 px).
#[cfg(windows)]
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
    /// Last video rectangle in screen coordinates (physical px), captured by
    /// `present`. The 16 ms wake poller and click router read this instead of
    /// asking the window for scale/origin on every tick.
    #[cfg_attr(not(windows), allow(dead_code))]
    last_video_rect: Option<(i32, i32, u32, u32)>,
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
    /// Region signature the frame was last forced to compose with. A caption
    /// composed into the window surface *before* the styles were fixed is only
    /// removed by re-composing the frame, and the bug is intermittent, so every
    /// show and every distinct region gets one forced composition. `None` after
    /// a hide, which makes the next show force it again.
    forced_region: Option<String>,
    /// Relocation budget for the margin layout: at most one move per frame,
    /// so a runaway event source cannot spin the UI thread.
    move_budget: MoveBudget,
    /// Set while `present` is inside a Win32 geometry call. Window events that
    /// arrive re-entrantly are dropped instead of starting another placement:
    /// this is the feedback-loop guard, and it is only consulted when the
    /// margin layout is on.
    applying_geometry: bool,
}

/// Managed state shared by the overlay commands, the window event handler and
/// the wake-up poller.
pub struct OverlayState {
    inner: Mutex<Inner>,
    wake_stop: Arc<AtomicBool>,
    /// Captured at construction on the Tauri main thread; used by the Windows
    /// frame-redraw path to avoid posting a redundant main-thread task.
    #[cfg_attr(not(windows), allow(dead_code))]
    main_thread: std::thread::ThreadId,
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
                last_video_rect: None,
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
                forced_region: None,
                move_budget: MoveBudget::default(),
                applying_geometry: false,
            }),
            wake_stop: Arc::new(AtomicBool::new(false)),
            main_thread: std::thread::current().id(),
        }
    }

    #[cfg(windows)]
    fn is_main_thread(&self) -> bool {
        self.main_thread == std::thread::current().id()
    }

    fn lock(&self) -> MutexGuard<'_, Inner> {
        self.inner
            .lock()
            .unwrap_or_else(|poison| poison.into_inner())
    }

    /// Force a frame + child repaint for a region that has not been composed
    /// since the last hide.
    ///
    /// The caption this fixes is intermittent: it is whatever frame happened to
    /// be composed while the window still carried one, and that composition can
    /// survive any number of style changes afterwards. So the rule is: every
    /// show, every wake from auto-hide, every distinct region, and after any
    /// strip that changed a style bit — never "only the first time", which
    /// would just bet on the bug appearing once. Repeated identical signatures
    /// are skipped, so a present loop cannot turn into a repaint loop.
    fn force_frame_if_changed(
        &self,
        controls: &WebviewWindow,
        bands: Bands,
        signature: &str,
        reason: &str,
    ) {
        if bands.top_height == 0 || bands.top_width == 0 {
            return;
        }
        {
            let mut inner = self.lock();
            if inner.forced_region.as_deref() == Some(signature) {
                return;
            }
            inner.forced_region = Some(signature.to_string());
        }
        #[cfg(windows)]
        if let Ok(hwnd) = controls_hwnd(controls) {
            if self.is_main_thread() {
                force_frame_redraw(hwnd, signature, reason);
            } else {
                let hwnd = hwnd as isize;
                let signature = signature.to_string();
                let reason = reason.to_string();
                if let Err(err) = controls.run_on_main_thread(move || {
                    force_frame_redraw(
                        hwnd as windows_sys::Win32::Foundation::HWND,
                        &signature,
                        &reason,
                    );
                }) {
                    eprintln!("[overlay] could not post frame redraw: {err}");
                }
            }
        }
        #[cfg(not(windows))]
        {
            let _ = (controls, reason);
        }
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

        // Either direction starts with a visible arrow: entering the player
        // shows the bar first; leaving must never leave the desktop cursor
        // hidden. Overlay visibility re-hides it once the bar fades.
        let app_for_cursor = app.clone();
        let _ = app.run_on_main_thread(move || {
            if let Some(service) = app_for_cursor.try_state::<crate::player::PlayerService>() {
                service.set_cursor_hidden(false);
            }
        });

        if player {
            let mut first_error = None;
            if let Err(err) = main.set_background_color(Some(Color(0, 0, 0, 255))) {
                first_error.get_or_insert_with(|| format!("set background: {err}"));
            }
            if let Err(err) = self.present(app) {
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
            if let Err(err) = self.present(app) {
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
            if let Err(err) = self.present(app) {
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
            if let Err(err) = self.present(app) {
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
                let Some(controls) = app.get_webview_window(CONTROLS_LABEL) else {
                    return;
                };
                let app_for_state = app.clone();
                let controls_for_main = controls.clone();
                if let Err(err) = controls.run_on_main_thread(move || {
                    let Some(state) = app_for_state.try_state::<OverlayState>() else {
                        return;
                    };
                    // Re-check on the main thread: the page may have turned
                    // input back on while this task was queued.
                    let still_wanted = {
                        let inner = state.lock();
                        !inner.interactive
                            && !inner.suspended
                            && inner.interaction_generation == generation
                    };
                    if still_wanted {
                        state.apply_click_through(&controls_for_main);
                    }
                }) {
                    eprintln!("[overlay] could not post click-through apply: {err}");
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
            Ok(()) => {
                let generation = {
                    let mut inner = self.lock();
                    inner.cursor_ignored = Some(ignore);
                    inner.interaction_generation
                };
                // `set_ignore_cursor_events` rebuilds the whole style from
                // tao's window flags, so the caption strip has to run again.
                reassert_controls_chrome(controls);
                // Every wake (the bar coming back from auto-hide) composes the
                // frame once more before the bar is shown again. The region
                // does not change on an auto-hide cycle, so this is the only
                // point that can guarantee a clean frame for a wake.
                if !ignore {
                    // The guard has to be dropped before the call: the body of
                    // an `if let` keeps the scrutinee's temporaries alive, and
                    // `force_frame_if_changed` starts by locking this same
                    // mutex. `std::sync::Mutex` is not reentrant, so the
                    // original `if let Some(bands) = self.lock().last_bands`
                    // deadlocked the UI thread the moment the bar became
                    // interactive (pause) — the app stopped pumping messages
                    // while mpv kept playing.
                    let bands = { self.lock().last_bands };
                    if let Some(bands) = bands {
                        let signature = format!("wake-{generation}");
                        self.force_frame_if_changed(controls, bands, &signature, "wake");
                    }
                }
            }
            Err(err) => eprintln!("[overlay] could not set click-through to {ignore}: {err}"),
        }
    }

    pub fn on_main_geometry(&self, app: &AppHandle) {
        if !self.is_player_mode() {
            return;
        }
        // Drop geometry events that our own placement caused. Only consulted
        // while the margin layout is active (`EMBY_TOP_MARGIN=0` turns it off
        // and with it this guard).
        if overlay_margin(1.0) > 0 && self.lock().applying_geometry {
            log_diag("geometry: on_main_geometry event dropped (applying_geometry)");
            return;
        }
        if let Err(err) = self.present(app) {
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
    /// Move the controls window to `geometry`, never holding the state lock
    /// across a Win32 call.
    ///
    /// With the margin layout this is where the anti-oscillation guards live:
    /// a relocation budget (`MAX_MOVES_PER_SECOND`) and the `applying_geometry`
    /// flag, which is set for the duration of the move so any window event the
    /// move itself causes is dropped instead of moving the window again.
    /// Every move is logged with its before/after rectangle while the layout is
    /// switched on.
    fn apply_geometry(
        &self,
        controls: &WebviewWindow,
        geometry: (i32, i32, u32, u32),
        margin: i32,
    ) -> Result<(), String> {
        if margin > 0 {
            let allowed = {
                let mut inner = self.lock();
                if inner.move_budget.allows(Instant::now()) {
                    inner.move_budget.record();
                    inner.applying_geometry = true;
                    true
                } else {
                    false
                }
            };
            if !allowed {
                log_diag(&format!(
                    "geometry move skipped: reason=budget-per-second target={geometry:?}"
                ));
                return Ok(());
            }
            #[cfg(windows)]
            let before = controls_hwnd(controls).ok().and_then(window_rect);
            let result = apply_bounds(controls, geometry);
            // Clear the guard before propagating any error: leaving it set would
            // block every later present.
            self.lock().applying_geometry = false;
            result?;
            #[cfg(windows)]
            if let Ok(hwnd) = controls_hwnd(controls) {
                diag::write(&format!(
                    "geometry move: reason=present target={geometry:?} before={before:?} after={:?}",
                    window_rect(hwnd)
                ));
            }
        } else {
            // Default path: same single SetWindowPos as before the feature.
            apply_bounds(controls, geometry)?;
        }
        self.lock().last_geometry = Some(geometry);
        Ok(())
    }

    /// The caller must be on the Tauri main thread: sync window commands and
    /// window events already are, and the warm-up fallback posts itself here.
    /// This keeps SetWindowPos/SetWindowRgn out of the wake/click workers.
    fn present(&self, app: &AppHandle) -> Result<(), String> {
        // Feedback-loop guard: a window event that arrives while we are inside
        // `SetWindowPos` must not start another placement. Only active while the
        // margin layout is on; `EMBY_TOP_MARGIN=0` disables both.
        if overlay_margin(1.0) > 0 && self.lock().applying_geometry {
            log_diag("geometry: re-entrant present dropped (applying_geometry)");
            return Ok(());
        }
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

        // The page's `devicePixelRatio` is the single source for the video
        // rect, the margin and the bands. Asking the window for scale_factor
        // separately made the two sides round differently during a DPI change.
        let scale = if sync.dpr.is_finite() && sync.dpr > 0.0 {
            sync.dpr
        } else {
            1.0
        };
        let client_origin = main
            .inner_position()
            .map_err(|err| format!("inner position: {err}"))?;
        let outer_top = main
            .outer_position()
            .map_err(|err| format!("outer position: {err}"))?
            .y;
        // The window keeps the expanded size; opening or closing a menu only
        // changes the window region (the visible, hit-testable bottom strip),
        // so the webview is never resized and cannot show a stale frame.
        let margin = overlay_margin(scale);
        let geometry = compute_geometry(client_origin, outer_top, scale, &sync);
        let bands = active_bands(geometry.2, geometry.3, scale, expanded);

        let (geometry_changed, region_changed) = {
            let inner = self.lock();
            // With the margin layout a sub-pixel wobble is not a reason to move:
            // `geometry_needs_move` only reports a change beyond the slack.
            let geometry_changed = if margin > 0 {
                geometry_needs_move(inner.last_geometry, geometry)
            } else {
                inner.last_geometry != Some(geometry)
            };
            (geometry_changed, geometry_changed || inner.last_bands != Some(bands))
        };
        if geometry_changed {
            self.apply_geometry(&controls, geometry, margin)?;
        }
        let applied = {
            let inner = self.lock();
            inner.last_geometry.unwrap_or(geometry)
        };
        let video_rect = (
            applied.0,
            applied.1.saturating_add(margin),
            applied.2,
            applied.3.saturating_sub(margin.max(0) as u32),
        );
        self.lock().last_video_rect = Some(video_rect);
        if region_changed {
            // Size the region from the rect the window actually has. If the move
            // budget skipped this target, `applied` is the previous geometry;
            // using the new target here would clip a window that never moved.
            apply_region(&controls, applied.2, applied.3, bands, margin)?;
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
            // With the margin layout the warm-up surface stops at the margin
            // too, or the non-client area is exposed exactly while the window
            // is being shown for the first time. Without the margin this is
            // the previous full-height region unchanged.
            let exposed_height = geometry.3.saturating_sub(margin.max(0) as u32);
            let full = Bands {
                top_width: geometry.2,
                top_height: exposed_height,
                bottom_height: exposed_height,
            };
            apply_region(&controls, geometry.2, geometry.3, full, margin)?;
            self.lock().last_bands = Some(full);
            show_controls(&controls)?;
            // Every show composes the frame again. The caption is intermittent,
            // so "once per playback" would be a bet; a full-height region
            // plus a show is one repaint, which is the cheapest guarantee that
            // the first visible frame of this show carries no stale caption.
            let shown_signature = format!("{}x{} full", geometry.2, geometry.3);
            self.force_frame_if_changed(&controls, full, &shown_signature, "show");
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
                let app_for_state = app.clone();
                if let Err(err) = app.run_on_main_thread(move || {
                    let Some(state) = app_for_state.try_state::<OverlayState>() else {
                        return;
                    };
                    if !state.lock().warmup_pending {
                        return;
                    }
                    if let Err(err) = state.present(&app_for_state) {
                        eprintln!("[overlay] warm-up fallback failed: {err}");
                    }
                }) {
                    eprintln!("[overlay] could not post warm-up fallback: {err}");
                }
            });
        } else if region_changed {
            // The same guarantee for a region that changes while the window is
            // already up (menu open/close, warm-up collapsing back to the bar):
            // compose it once for this distinct region before it is seen.
            let signature = format!(
                "{}x{} top={}x{} bottom={}",
                geometry.2, geometry.3, bands.top_width, bands.top_height, bands.bottom_height
            );
            self.force_frame_if_changed(&controls, bands, &signature, "region");
        }
        Ok(())
    }

    fn hide(&self, app: &AppHandle) {
        // No overlay means no video rectangle to wake against. The next
        // `present` captures it again.
        self.lock().last_video_rect = None;
        let was_shown = {
            let mut inner = self.lock();
            let shown = inner.controls_shown;
            inner.controls_shown = false;
            // A hidden window cannot be warmed up; the next show arms it again.
            // Without this the fallback thread could re-present a window the
            // page already left.
            inner.warmup_pending = false;
            // The next show has to compose the frame again (see
            // `force_frame_if_changed`).
            inner.forced_region = None;
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
                // `hide` rewrites the style from tao's flags just like `show`;
                // strip now so the next `present` cannot expose a caption that
                // was silently re-added while the window was invisible.
                reassert_controls_chrome(&controls);
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
        if let Err(err) = self.present(app) {
            eprintln!("[overlay] could not re-present controls after fullscreen: {err}");
        }
    }

    /// Cursor-vs-video test for the wake poller. Returns `false` when the rect
    /// is unavailable so a transient failure cannot wake a hidden overlay.
    /// This is geometry only; the poller separately requires the app to be
    /// foreground and the pointer to be over our own window hierarchy.
    #[cfg(windows)]
    fn cursor_inside_video(&self, cursor: (i32, i32)) -> bool {
        let (mode, sync, video_rect) = {
            let inner = self.lock();
            (inner.mode, inner.sync, inner.last_video_rect)
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
        let Some(video_rect) = video_rect else {
            return false;
        };
        point_in_rect(cursor, video_rect)
    }

    /// Screen-space test used by the click poller: true when the point is over
    /// the video and not over the (interactive) controls bar.
    #[cfg(windows)]
    fn is_stage_click(&self, app: &AppHandle, cursor: (i32, i32)) -> bool {
        let (mode, sync, shown, interactive, geometry, bands, video_rect) = {
            let inner = self.lock();
            (
                inner.mode,
                inner.sync,
                inner.controls_shown,
                inner.interactive,
                inner.last_geometry,
                inner.last_bands,
                inner.last_video_rect,
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
        let Some(video_rect) = video_rect else {
            return false;
        };
        if !point_in_rect(cursor, video_rect) {
            return false;
        }
        // Clicks on a visible strip belong to the controls window, not the
        // stage. Only the two strips the window region exposes count: anywhere
        // else the window is clipped and the click reaches the video.
        if shown && interactive {
            if let (Some((gx, gy, gw, gh)), Some(bands)) = (geometry, bands) {
                // Same rectangles the region exposes, mapped to screen space,
                // so clipping and click routing cannot drift apart. The
                // margin is the gap between the window top and the video top;
                // deriving it from the cached rect avoids another scale query.
                let margin = video_rect.1.saturating_sub(gy).max(0);
                let strips = region_rects(gw, gh, bands, margin);
                let hit = |(left, top, right, bottom): (i32, i32, i32, i32)| {
                    let left = gx.saturating_add(left);
                    let top = gy.saturating_add(top);
                    let right = gx.saturating_add(right);
                    let bottom = gy.saturating_add(bottom);
                    cursor.0 >= left
                        && cursor.0 < right
                        && cursor.1 >= top
                        && cursor.1 < bottom
                };
                if strips.iter().any(|&rect| hit(rect)) {
                    return false;
                }
            }
        }
        // The pointer must actually be over our own window hierarchy: clicking
        // another app that happens to sit above the video rect must not steal
        // focus.
        cursor_over_main(app, cursor)
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
) -> (i32, i32, u32, u32) {
    compute_geometry_with_margin(client_origin, outer_top, scale, sync, overlay_margin(scale))
}

/// `compute_geometry` with an explicit margin, so both layouts are testable
/// without touching the environment of the test process.
fn compute_geometry_with_margin(
    client_origin: PhysicalPosition<i32>,
    outer_top: i32,
    scale: f64,
    sync: &VideoSync,
    margin: i32,
) -> (i32, i32, u32, u32) {
    // The window covers the whole video rectangle. The main window's resize
    // band is handled by the HTML edge handles in the controls page, which
    // start a native resize of the main window; the overlay itself stays
    // full-bleed so the bar has no visible gutter.
    let x = client_origin.x + (sync.x * scale).round() as i32;
    let video_top = client_origin.y + (sync.y * scale).round() as i32;
    let width = (sync.w * scale).round().max(1.0) as u32;
    let video_bottom = video_top + (sync.h * scale).round().max(0.0) as i32;
    // Cover the whole video rectangle: the top-left return/logo strip and the
    // bottom transport surface are two regions of the same window, so the
    // top-left corner finally has a window to draw in. Never above the main
    // window and never outside its bottom edge; on a short window the surface
    // shrinks instead of hanging outside the video.
    let y = if margin > 0 {
        // Margin layout: lift the window so its non-client area sits above the
        // video rectangle and outside the region. The bottom edge stays where
        // it was, so only the height grows, by exactly this margin.
        video_top - margin
    } else {
        video_top.max(outer_top)
    };
    let bottom = video_bottom.max(y + 1);
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

/// Apply position and size in a single `SetWindowPos` so the bottom-anchored
/// overlay never paints at an intermediate rectangle while a menu expands.
/// The window's on-screen rectangle, for the relocation log.
#[cfg(windows)]
fn window_rect(hwnd: windows_sys::Win32::Foundation::HWND) -> Option<(i32, i32, i32, i32)> {
    use windows_sys::Win32::Foundation::RECT;
    use windows_sys::Win32::UI::WindowsAndMessaging::GetWindowRect;

    let mut rect = RECT {
        left: 0,
        top: 0,
        right: 0,
        bottom: 0,
    };
    let ok = unsafe { GetWindowRect(hwnd, &mut rect) };
    if ok == 0 {
        return None;
    }
    Some((rect.left, rect.top, rect.right, rect.bottom))
}

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

/// Compose the window frame and every child again, right now.
///
/// This is what removes a caption that DWM already composited into the window
/// surface *before* the style guard existed. Marking the window dirty is not
/// enough: `InvalidateRect` repaints the client area, while the caption lives
/// in the non-client frame of the surface the window was last composed with.
/// `RDW_FRAME | RDW_UPDATENOW | RDW_ALLCHILDREN` re-composes that frame and
/// the layered surface under it, synchronously, so no frame with the stale
/// caption is ever presented again.
///
/// Evidence: the replay of this bug had the style guard working (no window in
/// the process carried `WS_CAPTION`) and the white strip still visible until
/// this call was added back.
#[cfg(windows)]
fn force_frame_redraw(hwnd: windows_sys::Win32::Foundation::HWND, region: &str, reason: &str) {
    use windows_sys::Win32::Graphics::Gdi::{
        RedrawWindow, RDW_ALLCHILDREN, RDW_FRAME, RDW_INVALIDATE, RDW_UPDATENOW,
    };

    let result = unsafe {
        RedrawWindow(
            hwnd,
            std::ptr::null(),
            std::ptr::null_mut(),
            RDW_INVALIDATE | RDW_FRAME | RDW_UPDATENOW | RDW_ALLCHILDREN,
        )
    };
    diag::write(&format!(
        "frame forced redraw: region={region} reason={reason} result={result}"
    ));
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
    margin: i32,
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
        let [(top_left, top_top, top_right, top_bottom), (bottom_left, bottom_top, bottom_right, bottom_bottom)] =
            region_rects(width, height, bands, margin);
        let region = unsafe { CreateRectRgn(0, 0, 0, 0) };
        if region.is_null() {
            return Err("apply_region: CreateRectRgn failed".to_string());
        }
        let top_rect = unsafe { CreateRectRgn(top_left, top_top, top_right, top_bottom) };
        let bottom_rect = unsafe {
            CreateRectRgn(bottom_left, bottom_top, bottom_right, bottom_bottom)
        };
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
        // No forced `RedrawWindow` here. The strip in `strip_native_caption` is
        // what repaints a frame that was already composited, and it only runs
        // when there is actually something to remove; forcing a full frame +
        // child repaint on every region change is exactly the flash this
        // function was changed to avoid.
        diag::write(&format!(
            "region: window={}x{} margin={margin} top={},{},{},{} bottom={},{},{},{} SetWindowRgn={ok}",
            width, height, top_left, top_top, top_right, top_bottom, bottom_left, bottom_top,
            bottom_right, bottom_bottom,
        ));
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
        let _ = (controls, width, height, bands, margin);
        Ok(())
    }
}

// ---- window creation ---------------------------------------------------------

/// Create the hidden controls window during `setup`. A failure is logged by the
/// caller and must not prevent the library UI from running.
pub fn create_controls_window(app: &AppHandle) -> Result<WebviewWindow, String> {
    // Diagnostics first, before anything that could fail: from here on every
    // interesting step of this window's life lands in one file the user can send
    // back, whether or not the overlay ends up working.
    #[cfg(windows)]
    diag::init(app.path().app_config_dir().ok());
    #[cfg(windows)]
    diag::write(&format!(
        "controls window requested: build={} diag={}",
        diag::build_id(),
        diag::path()
            .map(|path| path.display().to_string())
            .unwrap_or_else(|| "<none>".to_string()),
    ));

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
    // The title is diagnostics, not decoration: the caption that keeps
    // reappearing is painted by Windows from exactly this string, so a
    // screenshot of the bug also proves which build produced it. `diag` puts the
    // same id in the log and into a small badge inside the controls page.
    // The build id stays in the title: it is what a screenshot of any future
    // chrome problem would show, without asking the user which binary they run.
    .title(format!("Emby Controls · {}", build_id()))
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
    // A line of small print in the controls page itself, so "still broken" and
    // "built from the fixed commit" can be checked from one screenshot. Injected
    // from here because the page is a separate, otherwise untouched webview.
    .on_page_load(|webview, _payload| {
        // One line of small print with the build id. It is the cheapest way to
        // tell a "still broken" report from a stale install: a screenshot carries
        // the commit that produced it. Deliberately faint, pointer-transparent
        // and outside the interactive rows.
        let script = format!(
            "(() => {{ const tag = \"embypc-build\"; \
             if (!document.body || document.getElementById(tag)) return; \
             document.body.style.paddingTop = \"{margin}px\"; \
             const node = document.createElement(\"div\"); \
             node.id = tag; \
             node.textContent = \"build {}\"; \
             node.style.cssText = \"position:fixed;left:8px;bottom:2px;z-index:2147483647;\
             pointer-events:none;font:9px/1.2 monospace;color:rgba(255,255,255,.28);\
             text-shadow:0 0 2px rgba(0,0,0,.6);\"; \
             document.body.appendChild(node); \
             }})()",
            build_id(),
            margin = overlay_margin_css(),
        );
        let _ = webview.eval(script);
    })
    .build()
    .map_err(|err| format!("controls window: {err}"))?;

    if let Err(err) = configure_controls_window(&window) {
        #[cfg(windows)]
        diag::write(&format!("configure failed: {err}"));
        let _ = window.destroy();
        return Err(err);
    }

    // The survey that rules other windows in or out as the source of a visible
    // artifact: every window in play, with its rectangle, region and style.
    #[cfg(windows)]
    if let (Ok(controls_raw), Ok(main_raw)) = (controls_hwnd(&window), main.hwnd()) {
        let main_raw = main_raw.0 as isize as windows_sys::Win32::Foundation::HWND;
        diag::write("--- window survey (once per launch) ---");
        diag::describe("main", main_raw);
        diag::describe("controls", controls_raw);
        diag::region("controls", controls_raw);
    }
    Ok(window)
}

fn configure_controls_window(window: &WebviewWindow) -> Result<(), String> {
    // Invisible until a video rect is known; do not intercept the mouse while
    // parked.
    window
        .set_ignore_cursor_events(true)
        .map_err(|err| format!("controls click-through: {err}"))?;

    // `focusable(false)` already keeps the window non-activating through
    // Tauri, so only `WS_EX_TOOLWINDOW` (keep the helper out of Alt+Tab) is
    // set here. tao has no flag for it and drops it on every rebuild, so the
    // caption guard re-adds it inside `WM_STYLECHANGING` instead of patching it
    // back after each call.
    #[cfg(windows)]
    apply_tool_window_style(window)?;

    // The window is created with a real Win32 caption (see
    // `harden_controls_window_chrome`); strip it before the window is ever
    // shown so no frame can be composited with it.
    harden_controls_window_chrome(window)?;
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
    window.show().map_err(|err| format!("show controls: {err}"))?;
    // Hard fallback, re-applied on every show: DWM must not render a
    // non-client frame for this window, and a DWM transition can drop that
    // setting. The invalidation is what makes the next composition happen
    // instead of re-using the frame DWM already has.
    #[cfg(windows)]
    if let Ok(hwnd) = controls_hwnd(window) {
        let hr = disable_dwm_nc_rendering(hwnd);
        let invalidated = unsafe {
            windows_sys::Win32::Graphics::Gdi::InvalidateRect(hwnd, std::ptr::null(), 1)
        };
        diag::write(&format!(
            "show chrome: dwm_nc_hr=0x{hr:08x} invalidate={invalidated}"
        ));
    }
    // `show` goes through tao's `WindowState::apply_diff`, which rebuilds
    // GWL_STYLE/GWL_EXSTYLE from the window flag set and puts `WS_CAPTION`
    // back (`to_window_styles` seeds it unconditionally for owned windows).
    reassert_controls_chrome(window);
    Ok(())
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
    strip_native_caption(hwnd, "main window")
}

// ---- controls diagnostics ----------------------------------------------------

/// Unconditional, file-based diagnostics for the controls window.
///
/// Three "the caption is still there" reports in a row, without any way to tell
/// whether the code meant to remove it ever ran, is why this exists. It cannot
/// depend on an environment variable (the application is started by
/// double-click, so a variable set in a shell is not inherited) and it cannot
/// use stderr (a release build is `windows_subsystem = "windows"` and has no
/// console at all).
///
/// Every line carries a millisecond timestamp (time since start plus wall
/// clock), the file is truncated on every launch so it only ever describes the
/// run that reproduced the problem, and it is cut back to [`MAX_BYTES`] if a
/// session manages to grow that far.
///
/// File: `<app config dir>/controls-diag.log`, i.e.
/// `%APPDATA%\com.ember.emby-desktop\controls-diag.log` on Windows.
#[cfg(windows)]
mod diag {
    use std::fs::{File, OpenOptions};
    use std::io::Write;
    use std::path::PathBuf;
    use std::sync::atomic::{AtomicU64, Ordering};
    use std::sync::{Mutex, OnceLock};
    use std::time::{Instant, SystemTime, UNIX_EPOCH};

    /// Rotate in place instead of growing without bound.
    pub const MAX_BYTES: u64 = 512 * 1024;

    static PATH: OnceLock<Option<PathBuf>> = OnceLock::new();
    static START: OnceLock<Instant> = OnceLock::new();
    static FILE: Mutex<Option<File>> = Mutex::new(None);
    static LINES: AtomicU64 = AtomicU64::new(0);

    /// Commit the binary was built from, injected by `build.rs`.
    pub fn build_id() -> &'static str {
        super::build_id()
    }

    /// `EMBY_DIAG=1` (or `true`) turns the diagnostic file on.
    fn enabled() -> bool {
        static ENABLED: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
        *ENABLED.get_or_init(|| crate::env_flag("EMBY_DIAG").unwrap_or(false))
    }

    pub fn path() -> Option<&'static PathBuf> {
        PATH.get().and_then(|path| path.as_ref())
    }

    /// Truncate the file and write the banner, and only when diagnostics were
    /// explicitly asked for with `EMBY_DIAG=1`.
    ///
    /// Opt-in on purpose: the log is a debugging tool, not part of normal
    /// operation, and a shipped package should not leave a file in the user's
    /// config directory. With the switch off nothing is created, nothing is
    /// written, and every write below returns immediately.
    ///
    /// Best effort throughout: a read-only directory may cost diagnostics,
    /// never the overlay.
    pub fn init(dir: Option<PathBuf>) {
        let path = if enabled() {
            dir.map(|dir| dir.join("controls-diag.log"))
        } else {
            None
        };
        let _ = PATH.set(path.clone());
        if let Some(path) = path.as_ref() {
            if let Some(parent) = path.parent() {
                let _ = std::fs::create_dir_all(parent);
            }
            // One launch, one file: what the user sends back always describes
            // the run that reproduced the problem.
            let _ = std::fs::remove_file(path);
        }
        let shown = match path.as_ref() {
            Some(path) => path.display().to_string(),
            None => "<no app config dir>".to_string(),
        };
        write(&format!(
            "=== controls diag start pid={} build={} file={shown} ===",
            std::process::id(),
            build_id(),
        ));
    }

    /// Append one timestamped line. Never panics, never blocks a window message
    /// for longer than the write itself.
    pub fn write(msg: &str) {
        let Some(Some(path)) = PATH.get() else {
            return;
        };
        let start = START.get_or_init(Instant::now);
        let rel = start.elapsed().as_millis();
        let epoch = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .map(|since| since.as_millis())
            .unwrap_or(0);
        let mut guard = match FILE.lock() {
            Ok(guard) => guard,
            Err(poison) => poison.into_inner(),
        };
        if guard.is_none() {
            *guard = OpenOptions::new().create(true).append(true).open(path).ok();
        }
        let Some(file) = guard.as_mut() else {
            return;
        };
        let _ = writeln!(file, "[+{rel:>7}ms {epoch}] {msg}");
        let _ = file.flush();
        let lines = LINES.fetch_add(1, Ordering::Relaxed) + 1;
        if lines % 256 == 0 {
            if let Ok(meta) = file.metadata() {
                if meta.len() > MAX_BYTES {
                    let _ = file.set_len(0);
                    let _ = writeln!(
                        file,
                        "[+{rel:>7}ms {epoch}] --- truncated at {MAX_BYTES} bytes ---"
                    );
                }
            }
        }
    }

    /// One line describing a window. Everything needed to decide whether a
    /// visible artifact could have been painted by it: class, title, style,
    /// extended style, rectangle and owning thread.
    pub fn describe(tag: &str, hwnd: windows_sys::Win32::Foundation::HWND) {
        use windows_sys::Win32::Foundation::RECT;
        use windows_sys::Win32::UI::WindowsAndMessaging::{
            GetClassNameW, GetWindowLongPtrW, GetWindowRect, GetWindowThreadProcessId,
            GetWindowTextW, GWL_EXSTYLE, GWL_STYLE,
        };

        if hwnd.is_null() {
            write(&format!("{tag}: <null hwnd>"));
            return;
        }
        let mut class = [0u16; 128];
        let mut title = [0u16; 256];
        let mut rect = RECT {
            left: 0,
            top: 0,
            right: 0,
            bottom: 0,
        };
        let mut class_text = String::new();
        let mut title_text = String::new();
        let (tid, style, ex_style) = unsafe {
            let class_len = GetClassNameW(hwnd, class.as_mut_ptr(), class.len() as i32);
            if class_len > 0 {
                class_text = utf16(&class[..class_len as usize]);
            }
            let title_len = GetWindowTextW(hwnd, title.as_mut_ptr(), title.len() as i32);
            if title_len > 0 {
                title_text = utf16(&title[..title_len as usize]);
            }
            let _ = GetWindowRect(hwnd, &mut rect);
            (
                GetWindowThreadProcessId(hwnd, std::ptr::null_mut()),
                GetWindowLongPtrW(hwnd, GWL_STYLE) as u32,
                GetWindowLongPtrW(hwnd, GWL_EXSTYLE) as u32,
            )
        };
        let raw = hwnd as isize;
        write(&format!(
            "{tag}: hwnd=0x{raw:x} tid={tid} class={class_text:?} title={title_text:?} \
             style=0x{style:08x} ex=0x{ex_style:08x} rect=({},{},{},{}) size={}x{}",
            rect.left,
            rect.top,
            rect.right,
            rect.bottom,
            rect.right - rect.left,
            rect.bottom - rect.top,
        ));
    }

    /// The window region as Windows actually holds it, plus the rectangle it
    /// falls back to when no region is set. The visible white strip has to be
    /// inside one of these to have any chance of coming from this window.
    pub fn region(tag: &str, hwnd: windows_sys::Win32::Foundation::HWND) {
        use windows_sys::Win32::Foundation::RECT;
        use windows_sys::Win32::Graphics::Gdi::GetWindowRgnBox;

        if hwnd.is_null() {
            return;
        }
        let mut box_rect = RECT {
            left: 0,
            top: 0,
            right: 0,
            bottom: 0,
        };
        let kind = unsafe { GetWindowRgnBox(hwnd, &mut box_rect) };
        // `GetWindowRgnBox` returns the region type and copies its bounding box,
        // which is what has to be compared against the visible white strip.
        write(&format!(
            "{tag}: region kind={kind} box=({},{},{},{}) size={}x{}",
            box_rect.left,
            box_rect.top,
            box_rect.right,
            box_rect.bottom,
            box_rect.right - box_rect.left,
            box_rect.bottom - box_rect.top,
        ));
    }


    /// `GetClassNameW`/`GetWindowTextW` fill a UTF-16 buffer.
    fn utf16(units: &[u16]) -> String {
        String::from_utf16_lossy(units)
    }
}

// ---- native caption guard ----------------------------------------------------
//
// Why this is a subclass and not another "strip after X" call.
//
// The previous attempt stripped `WS_CAPTION` after every Tauri call that was
// *believed* to rebuild the window style (`show`, `hide`,
// `set_ignore_cursor_events`). It did not hold, because that strip is a race,
// not an invariant:
//
// * tao rebuilds the whole `GWL_STYLE`/`GWL_EXSTYLE` from its flag set in
//   `WindowState::apply_diff`, and seventeen public setters reach it
//   (`set_visible`, `set_focusable`, `set_outer_position`, `set_inner_size`,
//   `set_resizable`, `set_minimizable`, `set_maximizable`, `set_closable`,
//   `set_ignore_cursor_events`, `set_minimized`, `set_maximized`,
//   `set_fullscreen`, `set_decorations`, `set_always_on_bottom`,
//   `set_always_on_top`, `set_rtl`, `set_undecorated_shadow`), plus one inside
//   tao's own `WM_DPICHANGED` handler (it clears `MAXIMIZED` when the inner
//   size changes) that application code can never hook. A list kept from the
//   outside cannot be complete.
// * `WebviewWindow::show()` is a request, not a call. tauri-runtime-wry runs
//   `WindowMessage::Show` inline on the event-loop thread, but posts it from any
//   other thread, and tao's `set_visible` itself defers through
//   `EventLoopThreadExecutor`. "Strip right after `show()` returned" is ordered
//   against machinery that has not run yet.
// * Even when the strip wins, `WS_CAPTION` had already been installed, so the
//   non-client area was recalculated and DWM composited a title bar. Clearing
//   the bit afterwards only schedules *another* recalculation, and the window is
//   `WS_EX_LAYERED` while it is click-through, so the painted caption can
//   survive in the layered surface until the frame is explicitly invalidated.
//
// So instead of winning the race, the bit is refused. `WM_STYLECHANGING` is
// sent to the window *before* `SetWindowLong(GWL_STYLE/GWL_EXSTYLE)` takes
// effect and hands the caller a `STYLESTRUCT` to modify. Clearing the caption
// there means the caption is never installed, whoever asks, from whichever
// thread, in whatever order. `WM_STYLECHANGED` is the backstop for a change
// that was already in flight when the guard went on, and `DwmSetWindowAttribute`
// stops DWM from rendering a non-client frame at all.

/// `WM_STYLECHANGING` and `WM_STYLECHANGED`; stable ABI values, declared here so
/// the constants sit next to the handler that depends on them.
#[cfg(windows)]
const WM_STYLECHANGING_MSG: u32 = 0x007C;
#[cfg(windows)]
const WM_STYLECHANGED_MSG: u32 = 0x007D;

/// `SetWindowSubclass` updates an existing `(proc, id)` pair instead of stacking
/// a second one, which is what makes installing this idempotent.
#[cfg(windows)]
const CHROME_SUBCLASS_ID: usize = 0x454D_4243; // "EMBC"

#[cfg(windows)]
static CAPTION_REFUSALS: AtomicU32 = AtomicU32::new(0);
/// One-shot flag so the "guard installed" line with the live style values is
/// printed once per process rather than on every re-assert.
#[cfg(windows)]
static CHROME_GUARD_LOGGED: AtomicBool = AtomicBool::new(false);

/// Print a chrome/style line and, when `EMBY_DIAG=1` is set, append it to
/// `%APPDATA%\com.ember.emby-desktop\controls-diag.log`.
///
/// The style values are the only runtime evidence that the caption guard is
/// doing its job, and a release build has no console at all
/// (`#![windows_subsystem = "windows"]` drops stderr), so the same lines have to
/// be reachable as a file the user can send along with a bug report. The file is
/// opt-in so a shipped package leaves nothing behind in the user's config
/// directory.
#[cfg(windows)]
fn log_chrome(line: &str) {
    eprintln!("{line}");
    diag::write(line);
}

/// The subclass procedure that owns the window style.
///
/// Everything it does not handle is forwarded with `DefSubclassProc`, so tao's
/// own window procedure (and any other subclass) keeps working unchanged.
#[cfg(windows)]
unsafe extern "system" fn caption_guard_proc(
    hwnd: windows_sys::Win32::Foundation::HWND,
    msg: u32,
    wparam: usize,
    lparam: isize,
    _id: usize,
    _data: usize,
) -> isize {
    use windows_sys::Win32::UI::WindowsAndMessaging::{
        SetCursor, GWL_EXSTYLE, GWL_STYLE, STYLESTRUCT, WM_SETCURSOR, WS_CAPTION, WS_DLGFRAME,
        WS_EX_TOOLWINDOW, WS_EX_WINDOWEDGE,
    };

    match msg {
        WM_STYLECHANGING_MSG => {
            let index = wparam as i32;
            let styles = lparam as *mut STYLESTRUCT;
            if !styles.is_null() {
                let old = (*styles).styleOld;
                let asked = (*styles).styleNew;
                if index == GWL_STYLE {
                    (*styles).styleNew &= !(WS_CAPTION | WS_DLGFRAME);
                } else if index == GWL_EXSTYLE {
                    // `WS_EX_TOOLWINDOW` keeps the helper window out of
                    // Alt+Tab. tao has no flag for it, so every rebuild drops
                    // it; forcing it here is what makes the one-time
                    // `apply_tool_window_style` stick.
                    (*styles).styleNew =
                        ((*styles).styleNew & !WS_EX_WINDOWEDGE) | WS_EX_TOOLWINDOW;
                }
                let applied = (*styles).styleNew;
                if applied != asked {
                    let seen = CAPTION_REFUSALS.fetch_add(1, Ordering::Relaxed) + 1;
                    // Only the first one: it proves the guard is installed and
                    // running, and after that a line per rebuild would be log
                    // spam on a window that toggles the bar all session.
                    if seen == 1 {
                        log_chrome(&format!(
                            "[overlay] caption guard active: old=0x{old:08x} refused 0x{asked:08x} -> 0x{applied:08x} (idx={index}, further refusals not logged)"
                        ));
                    }
                }
            }
        }
        WM_SETCURSOR if crate::video_window::cursor_is_hidden() => {
            SetCursor(std::ptr::null_mut());
            return 1;
        }
        WM_STYLECHANGED_MSG => {
            // Backstop only. If a change was already queued when the guard went
            // on, the bits can still be present here; take them off again. The
            // strip re-enters this procedure through WM_STYLECHANGING, which then
            // refuses the caption, so this terminates.
            let index = wparam as i32;
            let styles = lparam as *const STYLESTRUCT;
            if !styles.is_null()
                && index == GWL_STYLE
                && (*styles).styleNew & (WS_CAPTION | WS_DLGFRAME) != 0
            {
                diag::write(&format!(
                    "STYLECHANGED idx={index} style=0x{:08x} STILL HAS CAPTION",
                    (*styles).styleNew
                ));
                let _ = strip_native_caption(hwnd as isize, "controls window");
            }
        }
        _ => {}
    }
    windows_sys::Win32::UI::Shell::DefSubclassProc(hwnd, msg, wparam, lparam)
}

/// Ask DWM not to render a non-client frame for this window at all.
///
/// The style guard already stops a caption from being *installed*; this stops
/// DWM from composing a non-client frame from whatever it has cached, which is
/// the other half of the problem. It is re-applied after every show because a
/// DWM transition (fullscreen, restore, monitor change) can drop the setting.
/// Best effort: not required for the style guard, so a failure on an older
/// build is reported by the caller and otherwise ignored.
#[cfg(windows)]
fn disable_dwm_nc_rendering(hwnd: windows_sys::Win32::Foundation::HWND) -> i32 {
    use windows_sys::Win32::Graphics::Dwm::{
        DwmSetWindowAttribute, DWMWA_NCRENDERING_POLICY, DWMNCRP_DISABLED,
    };

    let policy: i32 = DWMNCRP_DISABLED;
    unsafe {
        DwmSetWindowAttribute(
            hwnd,
            DWMWA_NCRENDERING_POLICY as u32,
            &policy as *const i32 as *const core::ffi::c_void,
            std::mem::size_of::<i32>() as u32,
        )
    }
}
/// Put the guard on `hwnd` and switch DWM non-client rendering off.
///
/// Idempotent: `SetWindowSubclass` with the same `(proc, id)` updates the
/// existing entry, so the re-asserts further down can call this freely.
#[cfg(windows)]
fn install_caption_guard(hwnd: windows_sys::Win32::Foundation::HWND) -> Result<(), String> {
    use windows_sys::Win32::UI::Shell::SetWindowSubclass;

    let installed =
        unsafe { SetWindowSubclass(hwnd, Some(caption_guard_proc), CHROME_SUBCLASS_ID, 0) };
    if installed == 0 {
        // Both the API result and the error code: 87 (invalid parameter) would
        // mean a bad HWND, 0 with no error would mean the call never happened.
        let error = std::io::Error::last_os_error();
        diag::write(&format!(
            "guard: SetWindowSubclass -> 0 last_error={:?} ({error})",
            error.raw_os_error()
        ));
        return Err(format!("SetWindowSubclass failed: {error}"));
    }
    diag::write(&format!(
        "guard: SetWindowSubclass -> {installed} (last_error={:?})",
        std::io::Error::last_os_error().raw_os_error()
    ));

    let hr = disable_dwm_nc_rendering(hwnd);
    diag::write(&format!("guard: DwmSetWindowAttribute(NCRENDERING_POLICY) hr=0x{hr:08x}"));

    diag::describe("guard-installed", hwnd);
    diag::region("guard-installed", hwnd);
    if !CHROME_GUARD_LOGGED.swap(true, Ordering::Relaxed) {
        use windows_sys::Win32::UI::WindowsAndMessaging::{
            GetWindowLongPtrW, GWL_EXSTYLE, GWL_STYLE,
        };
        let style = unsafe { GetWindowLongPtrW(hwnd, GWL_STYLE) } as u32;
        let ex_style = unsafe { GetWindowLongPtrW(hwnd, GWL_EXSTYLE) } as u32;
        log_chrome(&format!(
            "[overlay] controls chrome guard installed: style 0x{style:08x} ex 0x{ex_style:08x}"
        ));
    }
    Ok(())
}

/// Remove the native caption/border bits tao always turns on.
///
/// tao's `WindowFlags::to_window_styles()` starts from
/// `WS_CAPTION | WS_CLIPSIBLINGS | WS_SYSMENU` and only strips `WS_CAPTION`
/// for `WS_CHILD` windows. Both windows here are frameless *owned* windows
/// (`Parent::OwnedBy` / `Parent::None`), so they come out of `CreateWindowExW`
/// with a real Win32 title bar that Windows paints in the system (light)
/// colour. `harden_main_window_chrome` has always handled that for the main
/// window. The controls window never needed it while its window region only
/// exposed the bottom strip — the caption sits at the top of the window and was
/// clipped away. Once the region started exposing the top-left band it became
/// visible as a white "half title bar" (icon + "Emby Controls") over the video.
///
/// Every `SetWindowLongPtrW(GWL_STYLE, ...)` triggers a non-client repaint, so
/// this must be re-run after any Tauri call that changes window flags:
/// `WindowState::apply_diff` in tao rebuilds the *whole* style from its flag
/// set on every change (`show`, `hide`, `set_ignore_cursor_events`), which puts
/// `WS_CAPTION` straight back.
#[cfg(windows)]
fn strip_native_caption(hwnd: isize, what: &str) -> Result<(), String> {
    use windows_sys::Win32::UI::WindowsAndMessaging::{
        SetWindowLongPtrW, GWL_EXSTYLE, GWL_STYLE, SWP_FRAMECHANGED, SWP_NOACTIVATE,
        SWP_NOMOVE, SWP_NOSIZE, SWP_NOZORDER, WS_CAPTION, WS_DLGFRAME, WS_EX_WINDOWEDGE,
    };
    use windows_sys::Win32::UI::WindowsAndMessaging::{SetWindowPos, GetWindowLongPtrW};

    if hwnd == 0 {
        return Err(format!("{what} HWND is null"));
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
        // The actual values, tagged with the moment they were taken: this is the
        // runtime evidence for "was the window still carrying a caption?", and it
        // must never print again on the controls window once the guard is on.
        log_chrome(&format!(
            "[overlay] {what} caption stripped: style 0x{style:08x} -> 0x{wanted_style:08x}, \
             ex 0x{ex_style:08x} -> 0x{wanted_ex:08x}"
        ));
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
        // A caption that was already composited is not removed by the style write
        // alone: recompose the frame (and every child) right now, otherwise the
        // surface keeps showing the title bar it was last composed with even
        // though the style no longer has a caption. `InvalidateRect` alone only
        // marks the client area dirty and leaves the non-client frame in place,
        // which is why the strip used to win the style fight and lose the pixel.
        force_frame_redraw(hwnd, "style", "strip");
        // Read back instead of trusting the write: if some future tao/wry call
        // restores the caption through a path that bypasses the helpers in this
        // file, the bug shows up as a log line on the machine that can reproduce
        // it rather than as a white strip someone has to notice in a screenshot.
        // Deliberately a warning and not an `Err`: destroying the controls window
        // because a cosmetic style bit survived would be worse than the strip.
        let check = GetWindowLongPtrW(hwnd, GWL_STYLE) as u32;
        if check & (WS_CAPTION | WS_DLGFRAME) != 0 {
            log_chrome(&format!("[overlay] {what} still carries a native caption (style 0x{check:08x})"));
        }
    }
    Ok(())
}

#[cfg(not(windows))]
pub fn harden_main_window_chrome(_hwnd: isize) -> Result<(), String> {
    Ok(())
}

/// The controls window has the exact same tao quirk as the main window: it is
/// an *owned* popup (`parent(&main)` maps to `Parent::OwnedBy`), so
/// `WindowFlags::to_window_styles()` seeds `WS_CAPTION | WS_SYSMENU` and never
/// strips it again. Until task-35 the window region only exposed the bottom
/// strip, and the caption sits at the *top* of the window, so it was clipped
/// away and nobody noticed. The top-left band now exposes that corner, and
/// Windows paints the leftovers of a real Win32 title bar over the video: the
/// app icon plus the window title ("Emby Controls").
///
/// It is worse than a cosmetic overlay: `WS_CAPTION` makes the non-client area
/// taller, so the WebView2 client rect starts below the caption and every band
/// in `apply_region` is offset from the HTML it is supposed to match.
#[cfg(windows)]
fn harden_controls_window_chrome(window: &WebviewWindow) -> Result<(), String> {
    let hwnd = controls_hwnd(window)?;
    // Order matters: the guard has to be in place *before* the current caption is
    // removed, so that from the first strip onwards nothing can put it back.
    // A failure to install it is reported loudly but is *not* fatal: the caller
    // destroys the controls window on `Err`, and losing the whole transport bar
    // over a title strip would be far worse than the strip coming back. Without
    // the guard the strips still run, so the old behaviour is the floor.
    if let Err(err) = install_caption_guard(hwnd) {
        log_chrome(&format!("[overlay] caption guard could not be installed: {err}"));
    }
    strip_native_caption(hwnd as isize, "controls window")
}

#[cfg(not(windows))]
fn harden_controls_window_chrome(_window: &WebviewWindow) -> Result<(), String> {
    Ok(())
}

/// Re-assert the controls chrome after a call that makes tao rebuild the window
/// style (`show`, `hide`, `set_ignore_cursor_events`).
///
/// This is no longer what keeps the caption off — `install_caption_guard` is, by
/// refusing the bits in `WM_STYLECHANGING` however late the rebuild arrives. The
/// strip here is a cheap consistency check: it is a no-op once the guard is on,
/// and when it is *not* a no-op it prints the style it found, which is exactly
/// the evidence needed if the guard ever misses a path.
///
/// Deliberately strip-only. The guard is installed once, from the thread that
/// created the window (`SetWindowSubclass` may only be called from that thread),
/// while this runs from wherever the caller happens to be, including the
/// click-through worker. The write is posted to the window thread so it is
/// ordered after any rebuild tao queued from the caller's thread.
fn reassert_controls_chrome(window: &WebviewWindow) {
    #[cfg(windows)]
    {
        let Ok(hwnd) = controls_hwnd(window).map(|hwnd| hwnd as isize) else {
            return;
        };
        let window = window.clone();
        let _ = window.run_on_main_thread(move || {
            if let Err(err) = strip_native_caption(hwnd, "controls window") {
                eprintln!("[overlay] could not strip the controls caption: {err}");
            }
        });
    }
    #[cfg(not(windows))]
    {
        let _ = window;
    }
}

// ---- cursor wake poller ------------------------------------------------------

/// Poll the cursor while the app runs: every 16 ms while the pointer is over
/// the video, 250 ms otherwise. The loop stays parked (no Win32 calls) outside
/// player UI mode; `stop_wake` is checked every tick and the thread is detached
/// so shutdown never waits on it. Besides waking the bar on pointer movement,
/// the same tick samples the left mouse button so a click on the native video
/// window (which swallows the real mouse event) still focuses the app and can
/// toggle fullscreen on a double-click.
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
                let inside = state.cursor_inside_video(cursor);
                if !inside {
                    // Re-entering the video starts a fresh movement baseline.
                    last = None;
                    interval = WAKE_POLL_IDLE;
                    continue;
                }
                interval = WAKE_POLL_ACTIVE;
                // A visible player is not necessarily the active one. Do not
                // wake its bar while another app owns the foreground, or while
                // another window covers the video rect. Keep the fast poll
                // rate: a click on the inactive video still has to reach
                // `on_stage_click`, which activates the app and wakes the bar
                // itself.
                if !foreground_is_ours() || !cursor_over_main(&app, cursor) {
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

/// Whether the foreground window belongs to this process.
///
/// The wake poller sees the video rectangle even when another application is
/// in front. Hover must only bring the controls back for the active player;
/// clicks are handled separately and still activate the app via
/// [`OverlayState::on_stage_click`].
#[cfg(windows)]
pub(crate) fn foreground_is_ours() -> bool {
    use windows_sys::Win32::UI::WindowsAndMessaging::{
        GetForegroundWindow, GetWindowThreadProcessId,
    };

    let foreground = unsafe { GetForegroundWindow() };
    if foreground.is_null() {
        return false;
    }
    let mut process_id = 0;
    unsafe {
        GetWindowThreadProcessId(foreground, &mut process_id);
    }
    process_id == std::process::id()
}

/// Whether `cursor` is actually over the main window (or one of its children),
/// rather than another window that happens to cover the video rectangle.
#[cfg(windows)]
fn cursor_over_main(app: &AppHandle, cursor: (i32, i32)) -> bool {
    use windows_sys::Win32::Foundation::{HWND, POINT};
    use windows_sys::Win32::UI::WindowsAndMessaging::{IsChild, WindowFromPoint};

    let Some(main) = app.get_webview_window(MAIN_LABEL) else {
        return false;
    };
    let Ok(main) = main.hwnd() else {
        return false;
    };
    let main = main.0 as isize as HWND;
    let under = unsafe { WindowFromPoint(POINT { x: cursor.0, y: cursor.1 }) };
    !under.is_null() && (under == main || unsafe { IsChild(main, under) } != 0)
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
    fn default_geometry_applies_the_top_margin() {
        // The margin layout is the shipped default: the window is lifted by
        // `NC_SAFE_MARGIN` logical pixels and grows by exactly that much.
        let sync = video(0.0, 40.0, 1280.0, 720.0);
        let shipped = compute_geometry(PhysicalPosition::new(100, 50), 50, 1.0, &sync);
        let explicit = compute_geometry_with_margin(
            PhysicalPosition::new(100, 50),
            50,
            1.0,
            &sync,
            NC_SAFE_MARGIN as i32,
        );
        assert_eq!(shipped, explicit);
        assert_eq!(shipped, (100, 50, 1280, 760));
        // Video top is at 90; the window top is 40 above it.
        assert_eq!(shipped.1 + NC_SAFE_MARGIN as i32, 90);
    }

    #[test]
    fn default_region_starts_at_the_margin() {
        let bands = active_bands(1280, 760, 1.0, false);
        let [top, bottom] = region_rects(1280, 760, bands, NC_SAFE_MARGIN as i32);
        assert_eq!(top, (0, 40, 620, 128));
        assert_eq!(bottom, (0, 664, 1280, 760));
    }

    #[test]
    fn margin_off_restores_the_old_geometry_and_region() {
        // `EMBY_TOP_MARGIN=0` has to reproduce the pre-margin build exactly.
        let plain = compute_geometry_with_margin(
            PhysicalPosition::new(100, 50),
            50,
            1.0,
            &video(0.0, 40.0, 1280.0, 720.0),
            0,
        );
        assert_eq!(plain, (100, 90, 1280, 720));

        let full_bleed = compute_geometry_with_margin(
            PhysicalPosition::new(100, 50),
            50,
            1.0,
            &video(0.0, 0.0, 1280.0, 720.0),
            0,
        );
        assert_eq!(full_bleed, (100, 50, 1280, 720));

        let bands = active_bands(1280, 720, 1.0, false);
        let [top, bottom] = region_rects(1280, 720, bands, 0);
        assert_eq!(top, (0, 0, 620, 88));
        assert_eq!(bottom, (0, 624, 1280, 720));
    }

    #[test]
    fn margin_layout_lifts_the_window_and_keeps_the_video_top() {
        let sync = video(0.0, 0.0, 1280.0, 720.0);
        let no_margin = compute_geometry_with_margin(
            PhysicalPosition::new(0, 0),
            0,
            1.0,
            &sync,
            0,
        );
        let with_margin = compute_geometry_with_margin(
            PhysicalPosition::new(0, 0),
            0,
            1.0,
            &sync,
            40,
        );
        // Same bottom edge, same width, window lifted by exactly the margin.
        assert_eq!(with_margin.1, no_margin.1 - 40);
        assert_eq!(with_margin.3, no_margin.3 + 40);
        assert_eq!(with_margin.2, no_margin.2);
        assert_eq!(with_margin.1 + with_margin.3 as i32, 720, "bottom edge kept");

        let bands = active_bands(with_margin.2, with_margin.3, 1.0, false);
        let [top, _] = region_rects(with_margin.2, with_margin.3, bands, 40);
        // First exposed row = window top + margin = the video's top edge.
        assert_eq!(top.1, 40);
        assert_eq!(with_margin.1 + top.1, 0);
    }

    #[test]
    fn geometry_hysteresis_ignores_one_pixel_of_jitter() {
        let target = (10, 20, 300, 200);
        assert!(geometry_needs_move(None, target));
        assert!(!geometry_needs_move(Some(target), target));
        assert!(!geometry_needs_move(Some((11, 21, 299, 201)), target));
        assert!(geometry_needs_move(Some((12, 20, 300, 200)), target));
        assert!(geometry_needs_move(Some((10, 22, 300, 200)), target));
        assert!(geometry_needs_move(Some((10, 20, 302, 200)), target));
        assert!(geometry_needs_move(Some((10, 20, 300, 202)), target));
    }

    #[test]
    fn move_budget_caps_moves_per_second() {
        let mut budget = MoveBudget::default();
        let start = Instant::now();
        let mut allowed = 0;
        for _ in 0..MAX_MOVES_PER_SECOND + 10 {
            if budget.allows(start) {
                budget.record();
                allowed += 1;
            }
        }
        assert_eq!(allowed, MAX_MOVES_PER_SECOND);
        // The next second starts a fresh budget.
        assert!(budget.allows(start + Duration::from_secs(1)));
    }

    #[test]
    fn point_in_rect_uses_half_open_bounds() {
        let rect = (10, 20, 30, 40);
        assert!(point_in_rect((10, 20), rect));
        assert!(point_in_rect((39, 59), rect));
        assert!(!point_in_rect((9, 20), rect));
        assert!(!point_in_rect((10, 19), rect));
        assert!(!point_in_rect((40, 20), rect));
        assert!(!point_in_rect((10, 60), rect));
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
