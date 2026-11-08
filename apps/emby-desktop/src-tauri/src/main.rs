#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

//! Emby Desktop shell. The only job of this file is wiring the player kernel
//! (`emby_mpv`) and its Win32 host window (`video_window`) into Tauri: managed
//! state, command registration, channel subscription and teardown order.
//! The Emby session/API commands live in `emby`.

mod emby;
mod player;
mod video_window;

use player::PlayerService;
use tauri::{Manager, RunEvent, WindowEvent};

fn main() {
    let app = tauri::Builder::default()
        .setup(|app| {
            let window = app
                .get_webview_window("main")
                .ok_or_else(|| "main window \"main\" is missing from tauri.conf.json".to_string())?;

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

            app.manage(PlayerService::new(host));
            // Session store lives in the app config dir; a failure here is
            // fatal because login/logout could not persist anything.
            app.manage(emby::EmbyService::new(app.handle())?);
            Ok(())
        })
        .on_window_event(|window, event| {
            // Stop mpv while the Tauri window still exists: `terminate_destroy`
            // must run before the parent HWND is destroyed.
            if matches!(event, WindowEvent::CloseRequested { .. }) {
                if let Some(service) = window.app_handle().try_state::<PlayerService>() {
                    service.shutdown();
                }
            }
        })
        .invoke_handler(tauri::generate_handler![
            emby::emby_login,
            emby::emby_logout,
            emby::emby_session,
            emby::emby_views,
            emby::emby_items,
            emby::emby_item,
            emby::emby_playback_target,
            emby::emby_report_playback,
            emby::emby_image_url,
            player::video_sync,
            player::player_open,
            player::player_set_paused,
            player::player_seek,
            player::player_set_volume,
            player::player_set_muted,
            player::player_set_track,
            player::player_stop,
            player::player_snapshot,
            player::player_toggle_fullscreen,
            player::player_subscribe,
        ])
        .build(tauri::generate_context!())
        .expect("failed to build Emby Desktop");

    app.run(|handle, event| {
        // Covers exits that do not go through the main window (menu quit,
        // `app.exit()`, last window destroyed by the OS).
        if let RunEvent::ExitRequested { .. } = event {
            if let Some(service) = handle.try_state::<PlayerService>() {
                service.shutdown();
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
