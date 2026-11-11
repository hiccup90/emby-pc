#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

//! Emby Desktop shell. The only job of this file is wiring the player kernel
//! (`emby_mpv`) and its Win32 host window (`video_window`) into Tauri: managed
//! state, command registration, channel subscription and teardown order.
//! The Emby session/API commands live in `emby`.

mod emby;
mod overlay;
mod player;
mod settings;
mod video_window;
mod window_state;

use player::PlayerService;
use tauri::{Manager, RunEvent, WindowEvent};

fn main() {
    let app = tauri::Builder::default()
        .setup(|app| {
            let window = app
                .get_webview_window("main")
                .ok_or_else(|| "main window \"main\" is missing from tauri.conf.json".to_string())?;

            // Restore the last normal window geometry before creating the
            // native video host. Player fullscreen geometry is deliberately
            // never persisted by window_state.
            match window_state::WindowState::new(app.handle()) {
                Ok(state) => {
                    app.manage(state);
                    if let Some(state) = app.try_state::<window_state::WindowState>() {
                        let window_handle = window.as_ref().window();
                        // Capture the configured default geometry before the
                        // user can maximize, so a first-run maximize has a
                        // normal rectangle to restore later.
                        state.track(&window_handle);
                        if let Err(err) = state.restore(&window_handle) {
                            eprintln!("[desktop] window state restore skipped: {err}");
                        }
                    }
                }
                Err(err) => {
                    eprintln!("[desktop] window state disabled: {err}");
                }
            }

            // Created once on the main thread; libmpv later embeds into it via
            // `--wid`. A failure must not abort the app: the library UI still
            // works, and `player_open` reports the error.
            let host = match main_window_hwnd(&window) {
                Ok(parent_hwnd) => match video_window::VideoHost::create(parent_hwnd) {
                    Ok(host) => Some(host),
                    Err(err) => {
                        eprintln!("[desktop] video embedding disabled: {err}");
                        None
                    }
                },
                Err(err) => {
                    eprintln!("[desktop] video embedding disabled: {err}");
                    None
                }
            };

            app.manage(PlayerService::new(host, app.path().app_config_dir().ok()));
            // Remove the residual Win11 caption/dialog bits that `decorations:
            // false` leaves in the window style; without this the DWM paints a
            // 1px system line along the top of the window (the reported gap when
            // the window is flush with the top of the screen).
            if let Some(hwnd) = chrome_hwnd_webview(&window) {
                if let Err(err) = overlay::harden_main_window_chrome(hwnd) {
                    eprintln!("[desktop] main window chrome hardening skipped: {err}");
                }
            }
            // The native video is a child HWND above WebView2, so the controls
            // live in their own transparent window, owned by the main window
            // (the OS keeps it above the owner and destroys it with it). A
            // failure here only disables the overlay, never the library/player
            // commands.
            // `EMBY_NO_OVERLAY=1` skips the controls window entirely: a
            // diagnostic switch for comparing GPU use with and without the
            // transparent window that sits on top of the video.
            if overlay_disabled() {
                eprintln!("[desktop] controls overlay disabled by EMBY_NO_OVERLAY");
            } else if let Err(err) = overlay::create_controls_window(app.handle()) {
                eprintln!("[desktop] controls overlay disabled: {err}");
            }
            app.manage(overlay::OverlayState::new());
            overlay::spawn_wake_poller(app.handle().clone());
            // Session store lives in the app config dir; a failure here is
            // fatal because login/logout could not persist anything.
            app.manage(emby::EmbyService::new(app.handle())?);
            Ok(())
        })
        .on_window_event(|window, event| {
            // Only the main window drives the player/overlay lifecycle; the
            // controls window is a decoration-less helper and must not run
            // `PlayerService::shutdown`.
            if window.label() != "main" {
                return;
            }
            let app = window.app_handle();
            match event {
                // Stop mpv while the Tauri window still exists:
                // `terminate_destroy` must run before the parent HWND is
                // destroyed.
                WindowEvent::CloseRequested { .. } => {
                    if let Some(state) = app.try_state::<window_state::WindowState>() {
                        state.save(window);
                    }
                    if let Some(service) = app.try_state::<PlayerService>() {
                        service.shutdown();
                    }
                    if let Some(state) = app.try_state::<overlay::OverlayState>() {
                        state.stop_wake();
                    }
                    // Owned windows are destroyed with their owner, but an
                    // explicit destroy keeps app exit immediate and avoids a
                    // stray controls surface while mpv tears down.
                    if let Some(controls) = app.get_webview_window(overlay::CONTROLS_LABEL) {
                        let _ = controls.destroy();
                    }
                }
                WindowEvent::Destroyed => {
                    if let Some(state) = app.try_state::<window_state::WindowState>() {
                        state.save(window);
                    }
                    if let Some(state) = app.try_state::<overlay::OverlayState>() {
                        state.on_main_destroyed();
                    }
                    // Covers programmatic destroys that never emitted
                    // CloseRequested (e.g. `app.exit()` paths).
                    if let Some(controls) = app.get_webview_window(overlay::CONTROLS_LABEL) {
                        let _ = controls.destroy();
                    }
                }
                // Focus is deliberately ignored: the overlay is an owned window
                // and visibility is driven by `player_ui_mode` / `video_sync`
                // only. Reacting to focus here caused a show/hide loop with the
                // overlay's own activation attempts.
                WindowEvent::Focused(_) => {}
                WindowEvent::Moved(_) | WindowEvent::Resized(_) => {
                    if let Some(state) = app.try_state::<window_state::WindowState>() {
                        state.track(window);
                    }
                    if let Some(state) = app.try_state::<overlay::OverlayState>() {
                        state.on_main_geometry(app, None);
                    }
                    // A fullscreen toggle hid both the native video and the
                    // overlay; the new client size only exists now. Re-place the
                    // video from the last known `VideoSync` and bring the bar
                    // back. Both calls are no-ops when nothing is pending, so
                    // ordinary moves/resizes stay cheap.
                    if let Some(service) = app.try_state::<PlayerService>() {
                        service.resync_video();
                    }
                    if let Some(state) = app.try_state::<overlay::OverlayState>() {
                        state.resume(app);
                    }
                    // tao rewrites the whole GWL_STYLE whenever it changes a
                    // window flag (fullscreen, maximize, restore), which puts
                    // the caption bits back. Re-assert the minimal style.
                    if let Some(hwnd) = chrome_hwnd(window) {
                        let _ = overlay::harden_main_window_chrome(hwnd);
                    }
                }
                WindowEvent::ScaleFactorChanged { scale_factor, .. } => {
                    if let Some(state) = app.try_state::<window_state::WindowState>() {
                        state.track(window);
                    }
                    if let Some(state) = app.try_state::<overlay::OverlayState>() {
                        state.on_main_geometry(app, Some(*scale_factor));
                    }
                    if let Some(hwnd) = chrome_hwnd(window) {
                        let _ = overlay::harden_main_window_chrome(hwnd);
                    }
                }
                _ => {}
            }
        })
        .invoke_handler(tauri::generate_handler![
            emby::emby_login,
            emby::emby_session_boot,
            emby::emby_logout,
            emby::emby_session,
            emby::emby_views,
            emby::emby_items,
            emby::emby_resume,
            emby::emby_item,
            emby::emby_similar,
            emby::emby_person_items,
            emby::emby_set_played,
            emby::emby_set_favorite,
            emby::emby_playback_target,
            emby::emby_report_playback,
            emby::emby_image_url,
            settings::settings_get,
            settings::settings_set,
            player::video_sync,
            player::player_open,
            player::player_set_paused,
            player::player_seek,
            player::player_set_volume,
            player::player_set_muted,
            player::player_set_track,
            player::player_stop,
            player::player_snapshot,
            player::player_stats,
            player::player_toggle_fullscreen,
            player::player_subscribe,
            overlay::player_ui_mode,
            overlay::overlay_set_expanded,
            overlay::overlay_set_interactive,
            overlay::overlay_drag_window,
        ])
        .build(tauri::generate_context!())
        .expect("failed to build Emby Desktop");

    app.run(|handle, event| {
        // Covers exits that do not go through the main window (menu quit,
        // `app.exit()`, last window destroyed by the OS).
        if let RunEvent::ExitRequested { .. } = event {
            if let Some(state) = handle.try_state::<window_state::WindowState>() {
                if let Some(window) = handle.get_webview_window("main") {
                    state.save(&window.as_ref().window());
                }
            }
            if let Some(service) = handle.try_state::<PlayerService>() {
                service.shutdown();
            }
            if let Some(state) = handle.try_state::<overlay::OverlayState>() {
                state.stop_wake();
            }
        }
    });
}

/// Parent HWND for the video host window (physical-pixel client area).
#[cfg(windows)]
fn main_window_hwnd(window: &tauri::WebviewWindow) -> Result<isize, String> {
    let hwnd = window
        .hwnd()
        .map_err(|err| format!("could not get the main window HWND: {err}"))?;
    // `windows::Win32::Foundation::HWND` is a newtype over `*mut c_void`.
    Ok(hwnd.0 as isize)
}

#[cfg(not(windows))]
fn main_window_hwnd(_window: &tauri::WebviewWindow) -> Result<isize, String> {
    Err("native video embedding (libmpv --wid) is only implemented on Windows".to_string())
}

/// `HWND` for the window-style fix, or `None` when it is unavailable.
///
/// `Window::hwnd()` is itself `#[cfg(windows)]`, so this needs a platform pair:
/// the caller (`setup` / `on_window_event`) stays platform-neutral and simply
/// skips the fix when there is no handle.
#[cfg(windows)]
fn chrome_hwnd(window: &tauri::Window) -> Option<isize> {
    match window.hwnd() {
        Ok(hwnd) => Some(hwnd.0 as isize),
        Err(err) => {
            eprintln!("[desktop] main window chrome hardening skipped: no HWND ({err})");
            None
        }
    }
}

#[cfg(not(windows))]
fn chrome_hwnd(_window: &tauri::Window) -> Option<isize> {
    None
}

#[cfg(windows)]
fn chrome_hwnd_webview(window: &tauri::WebviewWindow) -> Option<isize> {
    match window.hwnd() {
        Ok(hwnd) => Some(hwnd.0 as isize),
        Err(err) => {
            eprintln!("[desktop] main window chrome hardening skipped: no HWND ({err})");
            None
        }
    }
}

#[cfg(not(windows))]
fn chrome_hwnd_webview(_window: &tauri::WebviewWindow) -> Option<isize> {
    None
}

/// `EMBY_NO_OVERLAY=1` (1/true/yes/on) skips the controls window. Diagnostic
/// switch for comparing GPU use with and without the transparent window that
/// sits above the video; playback itself is unaffected.
fn overlay_disabled() -> bool {
    matches!(
        std::env::var("EMBY_NO_OVERLAY")
            .ok()
            .map(|value| value.trim().to_ascii_lowercase())
            .as_deref(),
        Some("1" | "true" | "yes" | "on")
    )
}
