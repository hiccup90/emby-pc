//! Error type shared by the loader, the FFI session and the Tauri layer.

use std::fmt;
use std::path::PathBuf;

/// One failed `LoadLibrary` / symbol resolution attempt.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct DllAttempt {
    pub path: PathBuf,
    /// Human readable detail, on Windows this includes the OS error code
    /// (e.g. "The specified module could not be found. (os error 126)").
    pub error: String,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum MpvError {
    /// Platform without a libmpv embedding implementation (non-Windows builds).
    Unsupported(String),
    /// No candidate DLL could be loaded.
    DllNotFound { attempts: Vec<DllAttempt> },
    /// A DLL loaded but a required symbol was missing (usually an ancient libmpv).
    MissingSymbol {
        library: PathBuf,
        symbol: &'static str,
        detail: String,
    },
    /// `mpv_client_api_version() >> 16 != 2`.
    IncompatibleApi { found: u64 },
    /// `mpv_create()` returned NULL.
    NullHandle,
    /// Any libmpv call returning a negative error code.
    Api {
        operation: &'static str,
        code: i32,
        message: String,
    },
    /// NUL byte in a string that must be passed to C.
    InteriorNul { context: &'static str },
    /// The player thread could not be spawned.
    ThreadSpawn(String),
    /// No player session is running (command could not be delivered).
    NotStarted,
}

impl MpvError {
    pub fn from_code(operation: &'static str, code: i32, message: impl Into<String>) -> Self {
        MpvError::Api {
            operation,
            code,
            message: message.into(),
        }
    }
}

impl fmt::Display for MpvError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            MpvError::Unsupported(detail) => write!(f, "libmpv is unsupported on this platform: {detail}"),
            MpvError::DllNotFound { attempts } => {
                write!(
                    f,
                    "failed to load libmpv-2.dll (mpv client API v{}.x). Tried:",
                    crate::ffi::EXPECTED_API_MAJOR
                )?;
                for attempt in attempts {
                    write!(f, "\n  - {}: {}", attempt.path.display(), attempt.error)?;
                }
                write!(
                    f,
                    "\nSet EMBY_MPV_DLL to an absolute path of a libmpv-2.dll build (mpv >= 0.36 is recommended)."
                )
            }
            MpvError::MissingSymbol {
                library,
                symbol,
                detail,
            } => write!(
                f,
                "libmpv at {} is missing required symbol {symbol} ({detail}); the DLL is too old or not libmpv",
                library.display()
            ),
            MpvError::IncompatibleApi { found } => write!(
                f,
                "incompatible libmpv client API: need major version {} (MPV_CLIENT_API_VERSION (2 << 16) | minor), found {:#x} (major {}, minor {})",
                crate::ffi::EXPECTED_API_MAJOR,
                found,
                (found >> 16) & 0xffff,
                found & 0xffff
            ),
            MpvError::NullHandle => write!(f, "mpv_create() returned a NULL handle"),
            MpvError::Api {
                operation,
                code,
                message,
            } => write!(f, "{operation} failed ({code}): {message}"),
            MpvError::InteriorNul { context } => {
                write!(f, "internal string contains a NUL byte ({context})")
            }
            MpvError::ThreadSpawn(detail) => write!(f, "failed to spawn the mpv thread: {detail}"),
            MpvError::NotStarted => write!(f, "the mpv player session is not running"),
        }
    }
}

impl std::error::Error for MpvError {}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn dll_error_lists_every_attempted_path() {
        let err = MpvError::DllNotFound {
            attempts: vec![
                DllAttempt {
                    path: PathBuf::from("C:\\app\\libmpv-2.dll"),
                    error: "os error 126".into(),
                },
                DllAttempt {
                    path: PathBuf::from("mpv-2.dll"),
                    error: "os error 126".into(),
                },
            ],
        };
        let text = err.to_string();
        assert!(text.contains("C:\\app\\libmpv-2.dll"));
        assert!(text.contains("mpv-2.dll"));
        assert!(text.contains("EMBY_MPV_DLL"));
    }

    #[test]
    fn api_version_error_reports_major_and_minor() {
        let err = MpvError::IncompatibleApi { found: (3 << 16) | 7 };
        let text = err.to_string();
        assert!(text.contains("major 3"), "{text}");
        assert!(text.contains("minor 7"), "{text}");
    }
}
