//! `X-Emby-Authorization` header assembly.
//!
//! Format (Emby/Jellyfin standard):
//! `MediaBrowser Client="...", Device="...", DeviceId="...", Version="..."`,
//! with `, Token="..."` for authenticated requests. Quotes and control
//! characters are stripped from the values so a hostname or token cannot break
//! the header or inject a new line.

/// Client name reported to the server.
pub const CLIENT_NAME: &str = "Emby Desktop";
/// Client version reported to the server; kept in sync with the crate version.
pub const CLIENT_VERSION: &str = env!("CARGO_PKG_VERSION");

/// Builds the `X-Emby-Authorization` value. `token` is optional: login and
/// `/System/Info/Public` are unauthenticated.
pub fn authorization_header(device_name: &str, device_id: &str, token: Option<&str>) -> String {
    let mut value = format!(
        "MediaBrowser Client=\"{}\", Device=\"{}\", DeviceId=\"{}\", Version=\"{}\"",
        escape(CLIENT_NAME),
        escape(device_name),
        escape(device_id),
        escape(CLIENT_VERSION)
    );
    if let Some(token) = token.map(str::trim).filter(|token| !token.is_empty()) {
        value.push_str(&format!(", Token=\"{}\"", escape(token)));
    }
    value
}

/// Best-effort device name shown in the Emby dashboard.
pub fn default_device_name() -> String {
    for key in ["COMPUTERNAME", "HOSTNAME"] {
        if let Ok(value) = std::env::var(key) {
            let value = value.trim();
            if !value.is_empty() {
                return value.to_string();
            }
        }
    }
    if cfg!(windows) {
        "Windows PC".to_string()
    } else {
        "PC".to_string()
    }
}

fn escape(value: &str) -> String {
    value
        .chars()
        .filter(|c| !c.is_control() && *c != '"' && *c != '\\')
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn builds_unauthenticated_header() {
        assert_eq!(
            authorization_header("Windows PC", "dev-123", None),
            "MediaBrowser Client=\"Emby Desktop\", Device=\"Windows PC\", DeviceId=\"dev-123\", Version=\"0.1.0\""
        );
    }

    #[test]
    fn appends_token_when_authenticated() {
        let header = authorization_header("PC", "dev-1", Some("token-abc"));
        assert!(header.ends_with(", Token=\"token-abc\""), "{header}");
        assert!(header.starts_with("MediaBrowser Client=\"Emby Desktop\""), "{header}");
        // An empty/whitespace token must not produce `Token=""`.
        assert!(!authorization_header("PC", "dev-1", Some("   ")).contains("Token"));
    }

    #[test]
    fn escapes_quotes_backslashes_and_control_characters() {
        let header = authorization_header("My \"PC\"\\\r\nX-Evil: 1", "dev\"1", None);
        assert!(!header.contains('\r'), "{header}");
        assert!(!header.contains('\n'), "{header}");
        assert_eq!(header.matches('"').count(), 8, "{header}");
        assert!(header.contains("Device=\"My PCX-Evil: 1\""), "{header}");
    }
}
