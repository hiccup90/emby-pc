//! Persistent main-window geometry (size, position and maximized state).
//!
//! This is intentionally separate from `session.json` (and from the session
//! module) and stores a tiny JSON file in the app config directory.
//!
//! Why not the official window-state plugin? Its move/resize listener saves
//! whatever geometry the window currently has, including the temporary
//! fullscreen geometry used by the player. The player must not overwrite the
//! normal app geometry, so this module tracks only normal (non-maximized,
//! non-fullscreen) geometry and saves on close. It also clamps restored
//! rectangles into a monitor work area, which handles unplugged monitors and
//! taskbars better than the plugin's corner-only intersection check.

use std::{fs, path::PathBuf, sync::Mutex};

use serde::{Deserialize, Serialize};
use tauri::{AppHandle, Manager, PhysicalPosition, PhysicalSize, Window};

const FILENAME: &str = "window-state.json";
const MIN_WIDTH: u32 = 520;
const MIN_HEIGHT: u32 = 320;

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
struct SavedWindowState {
    x: i32,
    y: i32,
    width: u32,
    height: u32,
    maximized: bool,
}

#[derive(Clone, Copy, Debug, Default)]
struct Tracked {
    normal: Option<SavedWindowState>,
    maximized: bool,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct WorkArea {
    x: i32,
    y: i32,
    width: u32,
    height: u32,
}

pub struct WindowState {
    path: PathBuf,
    tracked: Mutex<Tracked>,
}

impl WindowState {
    pub fn new(app: &AppHandle) -> Result<Self, String> {
        let dir = app
            .path()
            .app_config_dir()
            .map_err(|err| format!("无法定位窗口状态目录：{err}"))?;
        Ok(Self {
            path: dir.join(FILENAME),
            tracked: Mutex::new(Tracked::default()),
        })
    }

    /// Restore the saved normal rectangle, clamped into a monitor work area,
    /// then re-apply the maximized state. Missing state leaves the window's
    /// configured default size/position untouched.
    pub fn restore(&self, window: &Window) -> Result<(), String> {
        let Some(saved) = self.read()? else {
            return Ok(());
        };
        let monitors = window.available_monitors().map_err(|err| err.to_string())?;
        let areas = monitors.iter().map(work_area).collect::<Vec<_>>();
        if areas.is_empty() {
            return Ok(());
        }

        let fitted = fit_to_work_areas(saved, &areas);
        window
            .set_size(PhysicalSize::new(fitted.width, fitted.height))
            .map_err(|err| format!("无法恢复窗口尺寸：{err}"))?;
        window
            .set_position(PhysicalPosition::new(fitted.x, fitted.y))
            .map_err(|err| format!("无法恢复窗口位置：{err}"))?;
        if fitted.maximized {
            window
                .maximize()
                .map_err(|err| format!("无法恢复最大化状态：{err}"))?;
        }

        let mut tracked = self.tracked.lock().unwrap_or_else(|poison| poison.into_inner());
        tracked.normal = Some(SavedWindowState {
            maximized: false,
            ..fitted
        });
        tracked.maximized = fitted.maximized;
        Ok(())
    }

    /// Remember the last normal geometry. Fullscreen and maximized events are
    /// intentionally skipped: the player's fullscreen size must never replace
    /// the normal app size.
    pub fn track(&self, window: &Window) {
        let mut tracked = self.tracked.lock().unwrap_or_else(|poison| poison.into_inner());
        if window.is_fullscreen().unwrap_or(false) {
            return;
        }
        if window.is_maximized().unwrap_or(false) {
            tracked.maximized = true;
            return;
        }
        if window.is_minimized().unwrap_or(false) {
            return;
        }
        if let Some(current) = current_geometry(window) {
            tracked.normal = Some(current);
            tracked.maximized = false;
        }
    }

    /// Persist on close. If the window is fullscreen at close time, use the
    /// last normal geometry; if it is maximized, remember that flag but keep
    /// the normal rectangle as the restore target.
    pub fn save(&self, window: &Window) {
        let tracked = *self.tracked.lock().unwrap_or_else(|poison| poison.into_inner());
        let fullscreen = window.is_fullscreen().unwrap_or(false);
        let maximized = window.is_maximized().unwrap_or(tracked.maximized);
        let current = current_geometry(window);

        let saved = if fullscreen {
            tracked.normal.map(|mut state| {
                state.maximized = tracked.maximized;
                state
            })
        } else if maximized {
            tracked
                .normal
                .map(|mut state| {
                    state.maximized = true;
                    state
                })
                .or_else(|| {
                    current.map(|mut state| {
                        state.maximized = true;
                        state
                    })
                })
        } else if let Some(current) = current {
            Some(SavedWindowState {
                maximized: false,
                ..current
            })
        } else {
            // The window can already be destroyed when the synthetic
            // Destroyed/Exit events run; keep the last known maximized flag
            // instead of turning a saved maximized window into a normal one.
            tracked.normal.map(|mut state| {
                state.maximized = tracked.maximized;
                state
            })
        };

        if let Some(saved) = saved {
            if saved.width > 0 && saved.height > 0 {
                if let Err(err) = self.write(saved) {
                    eprintln!("[desktop] could not save window state: {err}");
                }
            }
        }
    }

    fn read(&self) -> Result<Option<SavedWindowState>, String> {
        match fs::read(&self.path) {
            Ok(bytes) => serde_json::from_slice(&bytes)
                .map(Some)
                .map_err(|err| format!("窗口状态文件无效：{err}")),
            Err(err) if err.kind() == std::io::ErrorKind::NotFound => Ok(None),
            Err(err) => Err(format!("无法读取窗口状态：{err}")),
        }
    }

    fn write(&self, state: SavedWindowState) -> Result<(), String> {
        let bytes = serde_json::to_vec_pretty(&state).map_err(|err| err.to_string())?;
        if let Some(parent) = self.path.parent() {
            fs::create_dir_all(parent).map_err(|err| format!("无法创建窗口状态目录：{err}"))?;
        }
        fs::write(&self.path, bytes).map_err(|err| format!("无法写入窗口状态：{err}"))
    }
}

fn current_geometry(window: &Window) -> Option<SavedWindowState> {
    let position = window.outer_position().ok()?;
    let size = window.inner_size().ok()?;
    if size.width == 0 || size.height == 0 {
        return None;
    }
    Some(SavedWindowState {
        x: position.x,
        y: position.y,
        width: size.width,
        height: size.height,
        maximized: false,
    })
}

fn work_area(monitor: &tauri::Monitor) -> WorkArea {
    let rect = monitor.work_area();
    WorkArea {
        x: rect.position.x,
        y: rect.position.y,
        width: rect.size.width,
        height: rect.size.height,
    }
}

fn intersection_area(a: WorkArea, b: WorkArea) -> u64 {
    let left = i64::from(a.x.max(b.x));
    let top = i64::from(a.y.max(b.y));
    let right = (i64::from(a.x) + i64::from(a.width))
        .min(i64::from(b.x) + i64::from(b.width));
    let bottom = (i64::from(a.y) + i64::from(a.height))
        .min(i64::from(b.y) + i64::from(b.height));
    let width = (right - left).max(0) as u64;
    let height = (bottom - top).max(0) as u64;
    width * height
}

fn fit_to_work_areas(saved: SavedWindowState, areas: &[WorkArea]) -> SavedWindowState {
    if areas.is_empty() {
        return saved;
    }
    let width = saved.width.max(MIN_WIDTH);
    let height = saved.height.max(MIN_HEIGHT);
    let saved_rect = WorkArea {
        x: saved.x,
        y: saved.y,
        width,
        height,
    };
    let best = areas
        .iter()
        .max_by_key(|area| intersection_area(saved_rect, **area))
        .copied();
    let (area, visible) = match best {
        Some(area) if intersection_area(saved_rect, area) > 0 => (area, true),
        _ => (areas[0], false),
    };
    let width = width.min(area.width.max(1));
    let height = height.min(area.height.max(1));
    let max_x = area.x + area.width as i32 - width as i32;
    let max_y = area.y + area.height as i32 - height as i32;
    let (x, y) = if visible {
        (saved.x.clamp(area.x, max_x), saved.y.clamp(area.y, max_y))
    } else {
        (
            area.x + (area.width as i32 - width as i32) / 2,
            area.y + (area.height as i32 - height as i32) / 2,
        )
    };
    SavedWindowState {
        x,
        y,
        width,
        height,
        maximized: saved.maximized,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn area(x: i32, y: i32, width: u32, height: u32) -> WorkArea {
        WorkArea { x, y, width, height }
    }

    fn saved(x: i32, y: i32, width: u32, height: u32) -> SavedWindowState {
        SavedWindowState { x, y, width, height, maximized: false }
    }

    #[test]
    fn offscreen_window_is_centered_on_a_monitor() {
        let fitted = fit_to_work_areas(saved(5000, 5000, 800, 600), &[area(0, 0, 1920, 1080)]);
        assert_eq!(fitted.x, 560);
        assert_eq!(fitted.y, 240);
        assert_eq!(fitted.width, 800);
        assert_eq!(fitted.height, 600);
    }

    #[test]
    fn oversized_window_is_clamped_to_the_work_area() {
        let fitted = fit_to_work_areas(saved(0, 0, 4000, 3000), &[area(0, 0, 1920, 1080)]);
        assert_eq!((fitted.x, fitted.y), (0, 0));
        assert_eq!((fitted.width, fitted.height), (1920, 1080));
    }

    #[test]
    fn negative_position_is_moved_into_the_visible_area() {
        let fitted = fit_to_work_areas(saved(-300, -200, 800, 600), &[area(0, 0, 1920, 1080)]);
        assert_eq!((fitted.x, fitted.y), (0, 0));
    }

    #[test]
    fn a_second_monitor_position_is_kept_when_visible() {
        let fitted = fit_to_work_areas(
            saved(2200, 100, 800, 600),
            &[area(0, 0, 1920, 1080), area(1920, 0, 1920, 1080)],
        );
        assert_eq!((fitted.x, fitted.y), (2200, 100));
    }
}
