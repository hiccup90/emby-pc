//! All libmpv options set **before** `mpv_initialize()`.
//!
//! Deliberately no `vo`, `gpu-api`, `gpu-context` or `gpu-sw`: libmpv picks its
//! default D3D11 VO when embedded with `wid`, and forcing those options is what
//! breaks HDR passthrough / tone-mapping on some drivers.
//!
//! `http-header-fields` is the only sensitive option (auth headers). libmpv
//! only accepts it before initialization; setting it at runtime writes the
//! value into mpv's log, so we never do that.
//!
//! `log-file` is opt-in ([`LOG_ENV_VAR`]). mpv forces at least `-v -v` for a
//! file log and writes the full stream URL ("Playing: <url>"); Emby stream URLs
//! carry `api_key`, so an always-on log would leave a token on disk for every
//! run.

use std::path::Path;

/// Set to `1`/`true`/`yes`/`on` to make libmpv write a file log.
pub const LOG_ENV_VAR: &str = "EMBY_MPV_LOG";

/// Whether a `LOG_ENV_VAR` value enables the file log. Pure so it can be
/// tested without touching the process environment.
pub fn log_enabled_from(value: Option<&str>) -> bool {
    matches!(
        value.map(|value| value.trim().to_ascii_lowercase()).as_deref(),
        Some("1" | "true" | "yes" | "on")
    )
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct MpvOption {
    pub name: String,
    pub value: String,
}

impl MpvOption {
    fn new(name: &str, value: impl Into<String>) -> Self {
        Self {
            name: name.to_string(),
            value: value.into(),
        }
    }
}

/// Options every session must have, in a stable order (index-based diffs help
/// when diagnosing a log). `host_hwnd` is the parent `HWND` for `--wid`.
/// `log_path` is `None` unless the user opted in via [`LOG_ENV_VAR`].
pub fn base_options(host_hwnd: u64, log_path: Option<&Path>) -> Vec<MpvOption> {
    let mut options = vec![
        MpvOption::new("config", "no"),
        MpvOption::new("terminal", "no"),
        MpvOption::new("input-default-bindings", "no"),
        MpvOption::new("input-vo-keyboard", "no"),
        MpvOption::new("osc", "no"),
        MpvOption::new("osd-bar", "no"),
        MpvOption::new("load-scripts", "no"),
        // --wid expects the HWND in decimal.
        MpvOption::new("wid", host_hwnd.to_string()),
        // D3D11VA hardware decoding without -copy: frames stay on the GPU.
        MpvOption::new("hwdec", "auto-safe"),
        MpvOption::new("keep-open", "yes"),
        MpvOption::new("idle", "yes"),
    ];
    if let Some(path) = log_path {
        options.push(MpvOption::new("log-file", path.to_string_lossy().into_owned()));
    }
    options
}

/// Like [`base_options`], but appends `http-header-fields` when the caller has
/// headers. Only valid for a freshly created handle (pre-initialize).
///
/// mpv expects a comma-separated list of `Name: value` pairs. Headers carrying
/// CR/LF are dropped to avoid option injection, and empty entries are ignored.
pub fn build_initial_options(
    host_hwnd: u64,
    log_path: Option<&Path>,
    headers: Option<&[String]>,
) -> Vec<MpvOption> {
    let mut options = base_options(host_hwnd, log_path);
    if let Some(value) = headers_option_value(headers) {
        options.push(MpvOption::new("http-header-fields", value));
    }
    options
}

/// Build the `http-header-fields` value, or `None` when nothing usable is left.
pub fn headers_option_value(headers: Option<&[String]>) -> Option<String> {
    let cleaned: Vec<&str> = headers
        .unwrap_or(&[])
        .iter()
        .map(|header| header.trim())
        .filter(|header| !header.is_empty())
        .filter(|header| !header.contains('\r') && !header.contains('\n'))
        .filter(|header| header.contains(':'))
        .collect();
    if cleaned.is_empty() {
        None
    } else {
        Some(cleaned.join(","))
    }
}

pub fn find_option<'a>(options: &'a [MpvOption], name: &str) -> Option<&'a str> {
    options
        .iter()
        .find(|option| option.name == name)
        .map(|option| option.value.as_str())
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::path::PathBuf;

    fn log_path() -> PathBuf {
        PathBuf::from(r"C:\Users\emby\AppData\Local\Temp\emby-desktop\mpv-4242.log")
    }

    #[test]
    fn base_option_set_is_exact_and_ordered() {
        let options = base_options(0x1234, None);
        let names: Vec<&str> = options.iter().map(|o| o.name.as_str()).collect();
        assert_eq!(
            names,
            [
                "config",
                "terminal",
                "input-default-bindings",
                "input-vo-keyboard",
                "osc",
                "osd-bar",
                "load-scripts",
                "wid",
                "hwdec",
                "keep-open",
                "idle",
            ]
        );
        assert_eq!(find_option(&options, "wid"), Some("4660")); // decimal HWND
        assert_eq!(find_option(&options, "hwdec"), Some("auto-safe"));
        assert_eq!(find_option(&options, "keep-open"), Some("yes"));
        assert_eq!(find_option(&options, "idle"), Some("yes"));
        assert_eq!(find_option(&options, "config"), Some("no"));
        assert_eq!(find_option(&options, "osc"), Some("no"));
        assert!(find_option(&options, "log-file").is_none());
    }

    #[test]
    fn log_file_is_only_set_when_requested() {
        let path = log_path();
        let options = base_options(0x1234, Some(path.as_path()));
        assert_eq!(options.last().unwrap().name, "log-file");
        assert_eq!(
            find_option(&options, "log-file"),
            Some(path.to_string_lossy().as_ref())
        );
        assert!(find_option(&base_options(0x1234, None), "log-file").is_none());
    }

    #[test]
    fn log_env_value_must_be_explicitly_truthy() {
        for on in ["1", "true", "TRUE", " yes ", "On"] {
            assert!(log_enabled_from(Some(on)), "{on:?} should enable the log");
        }
        for off in ["", "0", "false", "no", "off", "2", "enabled"] {
            assert!(!log_enabled_from(Some(off)), "{off:?} must not enable the log");
        }
        assert!(!log_enabled_from(None));
    }

    #[test]
    fn vo_and_gpu_options_are_left_to_mpv_defaults() {
        let options = base_options(0xdead_beef, None);
        for forbidden in ["vo", "gpu-api", "gpu-context", "gpu-sw", "hwdec-codecs"] {
            assert!(
                find_option(&options, forbidden).is_none(),
                "{forbidden} must not be forced"
            );
        }
    }

    #[test]
    fn hwdec_never_uses_copy_back() {
        let options = base_options(1, None);
        let hwdec = find_option(&options, "hwdec").unwrap();
        assert!(!hwdec.contains("copy"), "hwdec must stay zero-copy: {hwdec}");
        assert!(!hwdec.starts_with('-'));
    }

    #[test]
    fn headers_are_joined_and_none_by_default() {
        let no_headers = build_initial_options(1, None, None);
        assert!(find_option(&no_headers, "http-header-fields").is_none());

        let empty = build_initial_options(1, None, Some(&[]));
        assert!(find_option(&empty, "http-header-fields").is_none());

        let headers = vec![
            "Authorization: Bearer secret-token".to_string(),
            "X-Emby-Token: secret-token".to_string(),
            "   ".to_string(),
        ];
        let with_headers = build_initial_options(1, None, Some(&headers));
        assert_eq!(
            find_option(&with_headers, "http-header-fields"),
            Some("Authorization: Bearer secret-token,X-Emby-Token: secret-token")
        );
        // http-header-fields must come last, after every non-sensitive option
        // (including an opted-in log-file).
        assert_eq!(with_headers.last().unwrap().name, "http-header-fields");
        let path = log_path();
        let logged = build_initial_options(1, Some(path.as_path()), Some(&headers));
        assert_eq!(logged.last().unwrap().name, "http-header-fields");
    }

    #[test]
    fn header_injection_is_filtered() {
        let headers = vec![
            "Authorization: Bearer ok".to_string(),
            "X-Evil: value\r\nlog-file=oops".to_string(),
            "NoColonHere".to_string(),
        ];
        let value = headers_option_value(Some(&headers)).unwrap();
        assert_eq!(value, "Authorization: Bearer ok");
    }

    #[test]
    fn no_token_leaks_into_non_header_options() {
        // Simulates the "runtime setter leaks the token into mpv.log" pitfall:
        // with no headers requested, no value may contain header material.
        let path = log_path();
        let options = build_initial_options(1, Some(path.as_path()), None);
        for option in &options {
            assert!(!option.value.contains("Bearer"));
            assert!(!option.value.contains("Token"));
        }
    }

    #[test]
    fn all_option_values_are_non_empty_and_nul_free() {
        let path = log_path();
        let options = build_initial_options(
            42,
            Some(path.as_path()),
            Some(&["Authorization: Bearer abc".to_string()]),
        );
        for option in &options {
            assert!(!option.name.is_empty());
            assert!(!option.value.is_empty(), "{} has empty value", option.name);
            assert!(!option.name.contains('\0'));
            assert!(!option.value.contains('\0'));
        }
    }
}
