//! All libmpv options set **before** `mpv_initialize()`.
//!
//! The renderer is mpv's own default: on this libmpv build that is
//! `gpu-next`/libplacebo. Pinning `vo=gpu` to match Emby Theater's ancient
//! mpv 0.35 measured *worse* on an AMD 780M (3D engine 20 % → 32 %), so it is
//! left alone; mpv.conf can force any VO. `gpu-api`, `gpu-context` and
//! `gpu-sw` stay unset so the embedded D3D11 path is chosen by libmpv itself.
//!
//! The quality profile is supplied by the caller (`base_options`): the
//! low-overhead preset uses `profile=fast`, which on the same 780M took GPU use
//! from ~25 % to Emby Theater's level. It switches the scalers to bilinear and
//! turns off dithering, linear-light downscaling and deband. The "default"
//! preset passes `None`, leaving mpv's own quality defaults untouched.
//! Individual options can be overridden in mpv.conf (later options win).
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
/// `profile` is the optional mpv quality profile: `Some("fast")` for the
/// low-overhead preset, `None` for mpv's own default quality.
pub fn base_options(
    host_hwnd: u64,
    log_path: Option<&Path>,
    profile: Option<&str>,
) -> Vec<MpvOption> {
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
    ];
    // The cheap scaling/dithering path is opt-in: the low-overhead preset
    // passes `Some("fast")` (it cost ~10 points of 3D engine time on an AMD
    // 780M iGPU); the default quality preset passes `None` so mpv keeps its
    // own defaults. mpv.conf overrides still win (appended later).
    if let Some(profile) = profile.map(str::trim).filter(|value| !value.is_empty()) {
        options.push(MpvOption::new("profile", profile));
    }
    // Cover art must not be decoded as a video stream (Emby Theater does the same).
    options.push(MpvOption::new("audio-display", "no"));
    options.push(MpvOption::new("keep-open", "yes"));
    options.push(MpvOption::new("idle", "yes"));
    if let Some(path) = log_path {
        options.push(MpvOption::new("log-file", path.to_string_lossy().into_owned()));
    }
    options
}

/// Like [`base_options`], but appends `extra` (the user's `mpv.conf`) and then
/// `http-header-fields` when the caller has headers. Only valid for a freshly
/// created handle (pre-initialize). Later values win in mpv, so user options
/// override the built-in ones.
///
/// mpv expects a comma-separated list of `Name: value` pairs. Headers carrying
/// CR/LF are dropped to avoid option injection, and empty entries are ignored.
pub fn build_initial_options(
    host_hwnd: u64,
    log_path: Option<&Path>,
    profile: Option<&str>,
    extra: &[MpvOption],
    headers: Option<&[String]>,
) -> Vec<MpvOption> {
    let mut options = base_options(host_hwnd, log_path, profile);
    options.extend(extra.iter().cloned());
    if let Some(value) = headers_option_value(headers) {
        options.push(MpvOption::new("http-header-fields", value));
    }
    options
}

/// Parses the user's `mpv.conf`: one `name=value` per line, `#` or `;` starts a
/// comment, a leading `--` and one pair of surrounding quotes are accepted.
/// Invalid lines are reported instead of failing playback.
pub fn parse_user_options(text: &str) -> (Vec<MpvOption>, Vec<String>) {
    let mut options = Vec::new();
    let mut warnings = Vec::new();
    for (index, raw) in text.lines().enumerate() {
        let line = raw.trim();
        if line.is_empty() || line.starts_with('#') || line.starts_with(';') {
            continue;
        }
        let (name, value) = match line.split_once('=') {
            Some(parts) => parts,
            None => {
                warnings.push(format!("line {}: expected name=value", index + 1));
                continue;
            }
        };
        let name = name.trim().trim_start_matches('-');
        let mut value = value.trim();
        if value.len() >= 2 {
            let quoted = (value.starts_with('"') && value.ends_with('"'))
                || (value.starts_with('\'') && value.ends_with('\''));
            if quoted {
                value = &value[1..value.len() - 1];
            }
        }
        if name.is_empty() || name.chars().any(|c| c.is_whitespace() || c.is_control()) {
            warnings.push(format!("line {}: invalid option name", index + 1));
            continue;
        }
        options.push(MpvOption::new(name, value));
    }
    (options, warnings)
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
    fn base_option_set_is_exact_and_ordered_with_the_fast_profile() {
        let options = base_options(0x1234, None, Some("fast"));
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
                "profile",
                "audio-display",
                "keep-open",
                "idle",
            ]
        );
        assert_eq!(find_option(&options, "wid"), Some("4660")); // decimal HWND
        assert_eq!(find_option(&options, "hwdec"), Some("auto-safe"));
        assert_eq!(find_option(&options, "profile"), Some("fast"));
        assert_eq!(find_option(&options, "audio-display"), Some("no"));
        assert_eq!(find_option(&options, "keep-open"), Some("yes"));
        assert_eq!(find_option(&options, "idle"), Some("yes"));
        assert_eq!(find_option(&options, "config"), Some("no"));
        assert_eq!(find_option(&options, "osc"), Some("no"));
        assert!(find_option(&options, "log-file").is_none());
    }

    #[test]
    fn base_option_set_omits_profile_for_default_quality() {
        let options = base_options(0x1234, None, None);
        assert!(
            find_option(&options, "profile").is_none(),
            "default quality must not force an mpv profile"
        );
        // Blank/whitespace profile values are ignored as well.
        let blank = base_options(0x1234, None, Some("  "));
        assert!(find_option(&blank, "profile").is_none());
    }

    #[test]
    fn log_file_is_only_set_when_requested() {
        let path = log_path();
        let options = base_options(0x1234, Some(path.as_path()), None);
        assert_eq!(options.last().unwrap().name, "log-file");
        assert_eq!(
            find_option(&options, "log-file"),
            Some(path.to_string_lossy().as_ref())
        );
        assert!(find_option(&base_options(0x1234, None, None), "log-file").is_none());
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
    fn gpu_backend_options_are_left_to_mpv_defaults() {
        let options = base_options(0xdead_beef, None, None);
        // The renderer, the D3D11 backend and the quality profile stay at mpv's defaults.
        for forbidden in ["vo", "gpu-api", "gpu-context", "gpu-sw", "hwdec-codecs", "profile"] {
            assert!(
                find_option(&options, forbidden).is_none(),
                "{forbidden} must not be forced"
            );
        }
    }

    #[test]
    fn hwdec_never_uses_copy_back() {
        let options = base_options(1, None, None);
        let hwdec = find_option(&options, "hwdec").unwrap();
        assert!(!hwdec.contains("copy"), "hwdec must stay zero-copy: {hwdec}");
        assert!(!hwdec.starts_with('-'));
    }

    #[test]
    fn headers_are_joined_and_none_by_default() {
        let no_headers = build_initial_options(1, None, None, &[], None);
        assert!(find_option(&no_headers, "http-header-fields").is_none());

        let empty = build_initial_options(1, None, None, &[], Some(&[]));
        assert!(find_option(&empty, "http-header-fields").is_none());

        let headers = vec![
            "Authorization: Bearer secret-token".to_string(),
            "X-Emby-Token: secret-token".to_string(),
            "   ".to_string(),
        ];
        let with_headers = build_initial_options(1, None, Some("fast"), &[], Some(&headers));
        assert_eq!(
            find_option(&with_headers, "http-header-fields"),
            Some("Authorization: Bearer secret-token,X-Emby-Token: secret-token")
        );
        // http-header-fields must come last, after every non-sensitive option
        // (including an opted-in log-file).
        assert_eq!(with_headers.last().unwrap().name, "http-header-fields");
        let path = log_path();
        let logged = build_initial_options(1, Some(path.as_path()), None, &[], Some(&headers));
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
        let options = build_initial_options(1, Some(path.as_path()), None, &[], None);
        for option in &options {
            assert!(!option.value.contains("Bearer"));
            assert!(!option.value.contains("Token"));
        }
    }

    #[test]
    fn user_options_parse_and_override_built_ins() {
        let text = "# video\nvo=gpu-next\nhwdec = d3d11va\n\n; audio\naudio-channels=stereo\nprofile=\"fast\"\n--keep-open=no\nbroken line\n=oops\n";
        let (options, warnings) = parse_user_options(text);
        let names: Vec<&str> = options.iter().map(|o| o.name.as_str()).collect();
        assert_eq!(names, ["vo", "hwdec", "audio-channels", "profile", "keep-open"]);
        assert_eq!(find_option(&options, "vo"), Some("gpu-next"));
        assert_eq!(find_option(&options, "hwdec"), Some("d3d11va"));
        assert_eq!(find_option(&options, "profile"), Some("fast"));
        assert_eq!(warnings.len(), 2, "{warnings:?}");

        let merged = build_initial_options(1, None, Some("fast"), &options, None);
        // User options come after every built-in option, so mpv applies them last.
        let vo_values: Vec<&str> = merged
            .iter()
            .filter(|o| o.name == "vo")
            .map(|o| o.value.as_str())
            .collect();
        assert_eq!(vo_values, ["gpu-next"]);
        let idle = merged.iter().position(|o| o.name == "idle").unwrap();
        let last_hwdec = merged.iter().rposition(|o| o.name == "hwdec").unwrap();
        assert!(last_hwdec > idle, "user options come after every built-in");
    }

    #[test]
    fn user_mpv_conf_can_override_the_quality_profile() {
        let (options, _) = parse_user_options("profile=gpu-hq");
        let merged = build_initial_options(1, None, Some("fast"), &options, None);
        let profiles: Vec<&str> = merged
            .iter()
            .filter(|option| option.name == "profile")
            .map(|option| option.value.as_str())
            .collect();
        // Built-in preset first, user mpv.conf later: mpv applies the last one.
        assert_eq!(profiles, ["fast", "gpu-hq"]);
    }

    #[test]
    fn header_option_stays_last_with_user_options() {
        let (options, _) = parse_user_options("vo=gpu");
        let merged = build_initial_options(
            1,
            None,
            Some("fast"),
            &options,
            Some(&["Authorization: Bearer abc".to_string()]),
        );
        assert_eq!(merged.last().unwrap().name, "http-header-fields");
    }

    #[test]
    fn all_option_values_are_non_empty_and_nul_free() {
        let path = log_path();
        let options = build_initial_options(
            42,
            Some(path.as_path()),
            None,
            &[],
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