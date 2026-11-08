//! URL helpers: server normalisation, URL construction, `api_key` injection and
//! redaction for messages.
//!
//! All playback/image URLs have to be absolute and carry `api_key` because the
//! frontend hands them to `player_open`, which plays an arbitrary URL and does
//! not inject headers.

use url::Url;

use crate::error::{ApiError, ApiResult};

/// Normalises user input for the server address:
/// - trims whitespace,
/// - adds `http://` when no scheme was supplied,
/// - keeps only `http`/`https`,
/// - drops query/fragment and trailing slashes,
/// - rejects embedded credentials (they would end up in logs).
///
/// The path is preserved so a server hosted under a sub-path
/// (`https://example.com/emby`) keeps working.
pub fn normalize_server_url(input: &str) -> ApiResult<String> {
    let trimmed = input.trim();
    if trimmed.is_empty() {
        return Err(ApiError::config("请输入 Emby 服务器地址"));
    }

    let with_scheme = if has_scheme(trimmed) {
        trimmed.to_string()
    } else {
        format!("http://{trimmed}")
    };

    let mut url = Url::parse(&with_scheme).map_err(|_| {
        ApiError::config("Emby 服务器地址无效，请填写类似 http://192.168.1.10:8096 的地址")
    })?;

    if !matches!(url.scheme(), "http" | "https") {
        return Err(ApiError::config(
            "Emby 服务器地址必须以 http:// 或 https:// 开头",
        ));
    }
    if url.host_str().map(str::is_empty).unwrap_or(true) {
        return Err(ApiError::config("Emby 服务器地址缺少主机名"));
    }
    if !url.username().is_empty() || url.password().is_some() {
        return Err(ApiError::config("服务器地址不能包含用户名或密码"));
    }

    url.set_query(None);
    url.set_fragment(None);

    let path = url.path().trim_end_matches('/').to_string();
    url.set_path(if path.is_empty() { "/" } else { &path });

    let mut normalized = url.to_string();
    while normalized.ends_with('/') {
        normalized.pop();
    }
    Ok(normalized)
}

/// Builds an absolute URL by appending `segments` to `base` and encoding the
/// query pairs. Path segments are percent-encoded by the `url` crate, so item
/// ids and image types cannot break out of their segment.
pub fn build_url(base: &str, segments: &[&str], query: &[(&str, &str)]) -> ApiResult<String> {
    let server_error = || ApiError::config("Emby 服务器地址无效");
    let mut url = Url::parse(base).map_err(|_| server_error())?;
    {
        let mut path = url.path_segments_mut().map_err(|_| server_error())?;
        path.pop_if_empty();
        path.extend(segments.iter().copied());
    }
    if !query.is_empty() {
        let mut pairs = url.query_pairs_mut();
        for (key, value) in query {
            pairs.append_pair(key, value);
        }
    }
    Ok(url.into())
}

/// Turns a server-provided playback path into an absolute URL.
///
/// Emby usually answers with a path (`/videos/...`) but may also return an
/// absolute URL; both are accepted. The server base path is preserved for
/// sub-path deployments.
pub fn absolutize(base: &str, candidate: &str) -> String {
    let candidate = candidate.trim();
    if candidate.is_empty() {
        return base.trim_end_matches('/').to_string();
    }
    let lower = candidate.to_ascii_lowercase();
    if lower.starts_with("http://") || lower.starts_with("https://") {
        return candidate.to_string();
    }
    if let Some(rest) = candidate.strip_prefix("//") {
        let scheme = base
            .split_once("://")
            .map(|(scheme, _)| scheme)
            .unwrap_or("http");
        return format!("{scheme}://{rest}");
    }
    format!(
        "{}/{}",
        base.trim_end_matches('/'),
        candidate.trim_start_matches('/')
    )
}

/// Appends `api_key=<token>` unless the URL already carries one.
///
/// The token is percent-encoded. Existing query parameters are kept verbatim so
/// a server-generated `DirectStreamUrl` is not rewritten.
pub fn with_api_key(url: &str, token: &str) -> String {
    if token.trim().is_empty()
        || has_query_key(url, "api_key")
        || has_query_key(url, "X-Emby-Token")
    {
        return url.to_string();
    }

    let encoded: String = url::form_urlencoded::byte_serialize(token.as_bytes()).collect();
    let (base, fragment) = match url.split_once('#') {
        Some((base, fragment)) => (base, Some(fragment)),
        None => (url, None),
    };
    let separator = if base.contains('?') {
        if base.ends_with('?') || base.ends_with('&') {
            ""
        } else {
            "&"
        }
    } else {
        "?"
    };

    let mut result = format!("{base}{separator}api_key={encoded}");
    if let Some(fragment) = fragment {
        result.push('#');
        result.push_str(fragment);
    }
    result
}

/// Returns `scheme://host[:port]/path` with credentials, query and fragment
/// removed. Used for every URL that ends up in an error message or a log line.
pub fn redact_url(raw: &str) -> String {
    let stripped = strip_userinfo(raw);
    match Url::parse(&stripped) {
        Ok(mut url) => {
            url.set_query(None);
            url.set_fragment(None);
            let mut text = url.to_string();
            while text.ends_with('/') && text.matches('/').count() > 2 {
                text.pop();
            }
            text
        }
        Err(_) => "<无效地址>".to_string(),
    }
}

fn has_scheme(input: &str) -> bool {
    match input.split_once("://") {
        Some((scheme, _)) => {
            !scheme.is_empty()
                && scheme
                    .chars()
                    .all(|c| c.is_ascii_alphanumeric() || matches!(c, '+' | '-' | '.'))
        }
        None => false,
    }
}

fn has_query_key(url: &str, key: &str) -> bool {
    let Some((_, query)) = url.split_once('?') else {
        return false;
    };
    let query = query.split('#').next().unwrap_or(query);
    query.split('&').any(|pair| {
        let name = pair.split('=').next().unwrap_or("");
        name.eq_ignore_ascii_case(key)
    })
}

fn strip_userinfo(raw: &str) -> String {
    let Some(scheme_end) = raw.find("://") else {
        return raw.to_string();
    };
    let authority_start = scheme_end + 3;
    let rest = &raw[authority_start..];
    let authority_end = rest
        .find(['/', '?', '#'])
        .unwrap_or(rest.len());
    let authority = &rest[..authority_end];
    match authority.rfind('@') {
        Some(at) => format!("{}{}", &raw[..authority_start], &rest[at + 1..]),
        None => raw.to_string(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn normalizes_server_urls() {
        assert_eq!(
            normalize_server_url("  192.168.1.10:8096/// ").unwrap(),
            "http://192.168.1.10:8096"
        );
        assert_eq!(
            normalize_server_url("nas.local:8096").unwrap(),
            "http://nas.local:8096"
        );
        assert_eq!(
            normalize_server_url("https://example.com/emby/").unwrap(),
            "https://example.com/emby"
        );
        assert_eq!(
            normalize_server_url("HTTPS://Example.COM:8920").unwrap(),
            "https://example.com:8920"
        );
        assert_eq!(
            normalize_server_url("http://192.168.1.5:8096/?x=1#frag").unwrap(),
            "http://192.168.1.5:8096"
        );
    }

    #[test]
    fn rejects_invalid_server_urls() {
        assert!(normalize_server_url("").is_err());
        assert!(normalize_server_url("   ").is_err());
        assert!(normalize_server_url("not a url").is_err());
        assert!(normalize_server_url("ftp://example.com").is_err());
        assert!(normalize_server_url("http://user:pass@example.com").is_err());
    }

    #[test]
    fn builds_urls_with_and_without_subpath() {
        assert_eq!(
            build_url(
                "http://192.168.1.5:8096",
                &["Users", "u1", "Views"],
                &[]
            )
            .unwrap(),
            "http://192.168.1.5:8096/Users/u1/Views"
        );
        assert_eq!(
            build_url("https://example.com/emby", &["Users", "u1", "Items"], &[])
                .unwrap(),
            "https://example.com/emby/Users/u1/Items"
        );
        assert_eq!(
            build_url(
                "http://host",
                &["Items", "abc def", "Images", "Primary"],
                &[("maxWidth", "400")]
            )
            .unwrap(),
            "http://host/Items/abc%20def/Images/Primary?maxWidth=400"
        );
    }

    #[test]
    fn appends_api_key_once() {
        let direct = "http://host/videos/1/stream?static=true&mediaSourceId=s1";
        assert_eq!(
            with_api_key(direct, "tok"),
            "http://host/videos/1/stream?static=true&mediaSourceId=s1&api_key=tok"
        );
        assert_eq!(
            with_api_key("http://host/Items/1/Images/Primary", "tok"),
            "http://host/Items/1/Images/Primary?api_key=tok"
        );
        // Already present: untouched (case-insensitive key match).
        let already = "http://host/videos/1/stream?api_key=existing";
        assert_eq!(with_api_key(already, "tok"), already);
        let already_upper = "http://host/videos/1/stream?Api_Key=existing";
        assert_eq!(with_api_key(already_upper, "tok"), already_upper);
        // Empty token adds nothing.
        assert_eq!(with_api_key(direct, "  "), direct);
        // Fragments stay last.
        assert_eq!(
            with_api_key("http://host/a#frag", "t"),
            "http://host/a?api_key=t#frag"
        );
        // Token values are percent-encoded (space becomes '+' form encoding).
        assert_eq!(
            with_api_key("http://host/a", "a b&c"),
            "http://host/a?api_key=a+b%26c"
        );
    }

    #[test]
    fn absolutizes_playback_urls() {
        assert_eq!(
            absolutize(
                "http://host:8096",
                "/videos/1/stream?static=true&mediaSourceId=s1"
            ),
            "http://host:8096/videos/1/stream?static=true&mediaSourceId=s1"
        );
        assert_eq!(
            absolutize("https://example.com/emby", "/videos/1/stream"),
            "https://example.com/emby/videos/1/stream"
        );
        assert_eq!(
            absolutize("http://host", "https://cdn.example.com/v/1"),
            "https://cdn.example.com/v/1"
        );
        assert_eq!(
            absolutize("http://host", "//cdn.example.com/v/1"),
            "http://cdn.example.com/v/1"
        );
        assert_eq!(absolutize("http://host", "  "), "http://host");
    }

    #[test]
    fn redacts_credentials_query_and_fragment() {
        assert_eq!(
            redact_url("http://user:pass@host:8096/emby/Items/1?api_key=secret#frag"),
            "http://host:8096/emby/Items/1"
        );
        assert_eq!(
            redact_url("https://host/Videos/1/stream?static=true&api_key=secret"),
            "https://host/Videos/1/stream"
        );
        assert_eq!(redact_url("http://host:8096/"), "http://host:8096");
        assert_eq!(redact_url("not a url"), "<无效地址>");
        // The raw secret must never survive in the output.
        assert!(!redact_url("http://host/x?api_key=abc").contains("abc"));
    }
}
