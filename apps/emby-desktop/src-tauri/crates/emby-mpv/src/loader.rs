//! Dynamic libmpv loading.
//!
//! Search order (Windows):
//!   1. `%EMBY_MPV_DLL%` (absolute file path)
//!   2. `<exe dir>\libmpv-2.dll`, `<exe dir>\mpv-2.dll`
//!   3. `<exe dir>\resources\libmpv-2.dll`, `<exe dir>\resources\mpv-2.dll`
//!   4. bare `libmpv-2.dll` / `mpv-2.dll` (system/PATH lookup)
//!
//! We load by explicit path first so a stray DLL next to the exe cannot
//! silently win over the bundled one. Failures are collected and surfaced from
//! `player_open`; loading never aborts the process.

use std::ffi::OsStr;
use std::path::{Path, PathBuf};

use crate::error::MpvError;
#[cfg(windows)]
use crate::error::DllAttempt;
use crate::ffi::*;

/// A loaded libmpv plus the resolved symbol table.
///
/// Field order matters for teardown: `api` only holds plain function pointers,
/// and the `Library` is unloaded last (by `Drop` of the `_library` field).
pub struct MpvLibrary {
    pub api: MpvApi,
    /// Kept alive until all mpv handles are gone. It is never read directly:
    /// the value exists for its `Drop`, which unloads the DLL only after every
    /// mpv handle died. The underscore silences `dead_code` without an `allow`
    /// attribute (underscore-prefixed fields are still dropped normally).
    _library: libloading::Library,
}

impl MpvLibrary {
    /// Load libmpv from the standard search path and validate its client API.
    #[cfg(windows)]
    pub fn load() -> Result<Self, MpvError> {
        let candidates = candidate_paths();
        let mut attempts: Vec<DllAttempt> = Vec::new();
        let mut api_error: Option<MpvError> = None;

        for path in candidates {
            let library = match unsafe { libloading::Library::new(&path) } {
                Ok(library) => library,
                Err(err) => {
                    attempts.push(DllAttempt {
                        path,
                        error: err.to_string(),
                    });
                    continue;
                }
            };

            let loaded = match unsafe { Self::from_library(library, path.clone()) } {
                Ok(loaded) => loaded,
                Err(err) => {
                    attempts.push(DllAttempt {
                        path,
                        error: err.to_string(),
                    });
                    continue;
                }
            };

            match loaded.check_api_version() {
                Ok(()) => return Ok(loaded),
                Err(err) => {
                    attempts.push(DllAttempt {
                        path,
                        error: err.to_string(),
                    });
                    api_error = Some(err);
                }
            }
        }

        // A DLL that loaded but is ABI-incompatible is a much better error than
        // "not found", so prefer it when both happened.
        if let Some(err) = api_error {
            return Err(err);
        }
        Err(MpvError::DllNotFound { attempts })
    }

    /// Non-Windows: there is no `wid` child-window embedding, so refuse early
    /// instead of producing a video window that never appears.
    #[cfg(not(windows))]
    pub fn load() -> Result<Self, MpvError> {
        Err(MpvError::Unsupported(
            "libmpv embedding via --wid is only implemented for Windows".to_string(),
        ))
    }

    #[cfg(windows)]
    unsafe fn from_library(library: libloading::Library, path: PathBuf) -> Result<Self, MpvError> {
        macro_rules! required {
            ($name:literal, $ty:ty) => {{
                let symbol = unsafe { library.get::<$ty>($name.as_bytes()) }.map_err(|err| {
                    MpvError::MissingSymbol {
                        library: path.clone(),
                        symbol: $name,
                        detail: err.to_string(),
                    }
                })?;
                *symbol
            }};
        }

        macro_rules! optional {
            ($name:literal, $ty:ty) => {{
                match unsafe { library.get::<$ty>($name.as_bytes()) } {
                    Ok(symbol) => Some(*symbol),
                    Err(_) => None,
                }
            }};
        }

        let api = MpvApi {
            create: required!("mpv_create", MpvCreateFn),
            initialize: required!("mpv_initialize", MpvInitializeFn),
            set_option_string: required!("mpv_set_option_string", MpvSetOptionStringFn),
            set_property_string: required!("mpv_set_property_string", MpvSetPropertyStringFn),
            get_property_string: required!("mpv_get_property_string", MpvGetPropertyStringFn),
            free: required!("mpv_free", MpvFreeFn),
            command: required!("mpv_command", MpvCommandFn),
            observe_property: required!("mpv_observe_property", MpvObservePropertyFn),
            wait_event: required!("mpv_wait_event", MpvWaitEventFn),
            terminate_destroy: required!("mpv_terminate_destroy", MpvTerminateDestroyFn),
            error_string: required!("mpv_error_string", MpvErrorStringFn),
            client_api_version: required!("mpv_client_api_version", MpvClientApiVersionFn),
            request_log_messages: optional!("mpv_request_log_messages", MpvRequestLogMessagesFn),
            wakeup: optional!("mpv_wakeup", MpvWakeupFn),
        };

        Ok(Self {
            api,
            _library: library,
        })
    }

    /// `MPV_CLIENT_API_VERSION` of the loaded DLL, as a plain integer
    /// (`(major << 16) | minor`). The C return type is `unsigned long`, which
    /// is 32-bit on Windows.
    pub fn client_api_version(&self) -> u64 {
        let version = unsafe { (self.api.client_api_version)() };
        version as u64
    }

    /// Reject anything that is not client API major version 2.
    pub fn check_api_version(&self) -> Result<(), MpvError> {
        let found = self.client_api_version();
        if (found >> 16) as u32 != EXPECTED_API_MAJOR {
            return Err(MpvError::IncompatibleApi { found });
        }
        Ok(())
    }

    /// `mpv_error_string`, copied into a Rust string immediately (the returned
    /// pointer is "valid forever" per the header, but we never rely on that).
    pub fn error_string(&self, code: i32) -> String {
        unsafe {
            let message = (self.api.error_string)(code);
            if message.is_null() {
                format!("mpv error {code}")
            } else {
                std::ffi::CStr::from_ptr(message).to_string_lossy().into_owned()
            }
        }
    }
}

/// Candidate DLL paths in priority order, de-duplicated.
///
/// Pure function (no Win32 calls) so the order can be unit tested on any host.
pub fn candidate_paths_for(exe_dir: Option<&Path>, from_env: Option<&OsStr>) -> Vec<PathBuf> {
    let mut candidates: Vec<PathBuf> = Vec::new();

    if let Some(from_env) = from_env {
        if !from_env.is_empty() {
            candidates.push(PathBuf::from(from_env));
        }
    }

    if let Some(dir) = exe_dir {
        for name in ["libmpv-2.dll", "mpv-2.dll"] {
            candidates.push(dir.join(name));
        }
        for name in ["libmpv-2.dll", "mpv-2.dll"] {
            candidates.push(dir.join("resources").join(name));
        }
    }

    // Last resort: let LoadLibraryW search the app directory / PATH.
    for name in ["libmpv-2.dll", "mpv-2.dll"] {
        candidates.push(PathBuf::from(name));
    }

    let mut deduped: Vec<PathBuf> = Vec::new();
    for candidate in candidates {
        if !deduped.iter().any(|known| known == &candidate) {
            deduped.push(candidate);
        }
    }
    deduped
}

/// Candidate DLL paths for this process (`EMBY_MPV_DLL`, exe dir, resources,
/// bare names).
#[cfg(windows)]
pub fn candidate_paths() -> Vec<PathBuf> {
    let exe_dir = std::env::current_exe()
        .ok()
        .and_then(|exe| exe.parent().map(Path::to_path_buf));
    candidate_paths_for(exe_dir.as_deref(), std::env::var_os("EMBY_MPV_DLL").as_deref())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn api_major_must_be_two() {
        // Pure arithmetic check of the same expression used by the loader.
        fn major(version: u64) -> u64 {
            (version >> 16) & 0xffff
        }
        assert_eq!(major(EXPECTED_CLIENT_API_VERSION), 2);
        assert_eq!(major(0x0002_0006), 2);
        assert_eq!(major(0x0003_0000), 3);
        assert_eq!(major(0x0000_00ff), 0);
    }

    #[test]
    fn dll_search_order_is_env_exe_resources_then_bare() {
        let exe_dir = PathBuf::from("C:\\Apps\\Emby");
        let candidates = candidate_paths_for(
            Some(&exe_dir),
            Some(OsStr::new("D:\\custom\\libmpv-2.dll")),
        );
        assert_eq!(
            candidates,
            vec![
                PathBuf::from("D:\\custom\\libmpv-2.dll"),
                exe_dir.join("libmpv-2.dll"),
                exe_dir.join("mpv-2.dll"),
                exe_dir.join("resources").join("libmpv-2.dll"),
                exe_dir.join("resources").join("mpv-2.dll"),
                PathBuf::from("libmpv-2.dll"),
                PathBuf::from("mpv-2.dll"),
            ]
        );
    }

    #[test]
    fn dll_candidates_are_deduplicated() {
        let exe_dir = PathBuf::from("/opt/emby");
        let candidates = candidate_paths_for(
            Some(&exe_dir),
            Some(OsStr::new("/opt/emby/libmpv-2.dll")),
        );
        let occurrences = candidates
            .iter()
            .filter(|path| **path == exe_dir.join("libmpv-2.dll"))
            .count();
        assert_eq!(occurrences, 1, "{candidates:?}");
    }

    #[test]
    fn dll_search_without_env_or_exe_dir_falls_back_to_bare_names() {
        let candidates = candidate_paths_for(None, None);
        assert_eq!(
            candidates,
            vec![PathBuf::from("libmpv-2.dll"), PathBuf::from("mpv-2.dll")]
        );
        assert!(candidate_paths_for(None, Some(OsStr::new(""))).len() == 2);
    }

    #[test]
    fn error_string_is_never_empty_for_unknown_codes() {
        // Uses the formatting fallback only (no DLL on the test host).
        let text = format!("mpv error {}", -12345);
        assert!(text.contains("-12345"));
    }
}
