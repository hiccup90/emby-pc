//! Error type shared by every `emby-api` call.
//!
//! Messages are meant to be shown in the UI: short, Chinese and free of
//! secrets. URLs included in messages are passed through [`redact_url`], which
//! removes the query string (where `api_key` lives) and URL credentials.

use std::fmt;

use crate::url::redact_url;

/// Broad classification of an API failure. Callers mostly care about
/// [`ApiError::is_auth`] (session expired / invalid credentials) and about the
/// message; the kind is available for logging and tests.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ApiErrorKind {
    /// Bad input, missing session or an unusable local config directory.
    Config,
    /// HTTP 401/403: the caller must drop the stored session.
    Unauthorized,
    /// HTTP 404.
    NotFound,
    /// Other non-success HTTP status.
    Server,
    /// Connection failure, timeout or a broken response body.
    Transport,
    /// The response was not valid Emby JSON.
    Decode,
    /// PlaybackInfo returned no source the client can play without transcoding.
    NoPlayableSource,
    /// Unknown playback event name (frontend bug).
    Unsupported,
}

/// Failure returned by every fallible `emby-api` operation.
#[derive(Debug, Clone)]
pub struct ApiError {
    kind: ApiErrorKind,
    status: Option<u16>,
    message: String,
}

/// Convenience alias used across the crate.
pub type ApiResult<T> = Result<T, ApiError>;

impl ApiError {
    pub fn new(kind: ApiErrorKind, message: impl Into<String>) -> Self {
        Self {
            kind,
            status: None,
            message: message.into(),
        }
    }

    pub fn config(message: impl Into<String>) -> Self {
        Self::new(ApiErrorKind::Config, message)
    }

    pub fn transport(message: impl Into<String>) -> Self {
        Self::new(ApiErrorKind::Transport, message)
    }

    pub fn decode(message: impl Into<String>) -> Self {
        Self::new(ApiErrorKind::Decode, message)
    }

    pub fn unsupported(message: impl Into<String>) -> Self {
        Self::new(ApiErrorKind::Unsupported, message)
    }

    pub fn no_playable_source(message: impl Into<String>) -> Self {
        Self::new(ApiErrorKind::NoPlayableSource, message)
    }

    /// Maps a non-success HTTP status to a readable error.
    ///
    /// `url` is only ever embedded after [`redact_url`], so a token that leaked
    /// into a query string cannot reach the frontend or a log line.
    pub fn http_status(status: u16, url: &str) -> Self {
        let target = redact_url(url);
        let (kind, message) = match status {
            401 | 403 => (
                ApiErrorKind::Unauthorized,
                "登录已失效，请重新登录".to_string(),
            ),
            404 => (
                ApiErrorKind::NotFound,
                format!("找不到请求的 Emby 资源（HTTP 404）：{target}"),
            ),
            400..=499 => (
                ApiErrorKind::Server,
                format!("Emby 拒绝了请求（HTTP {status}）：{target}"),
            ),
            500..=599 => (
                ApiErrorKind::Server,
                format!("Emby 服务器出错（HTTP {status}）：{target}"),
            ),
            _ => (
                ApiErrorKind::Server,
                format!("Emby 请求失败（HTTP {status}）：{target}"),
            ),
        };
        Self {
            kind,
            status: Some(status),
            message,
        }
    }

    pub fn status(&self) -> Option<u16> {
        self.status
    }

    pub fn kind(&self) -> ApiErrorKind {
        self.kind
    }

    pub fn message(&self) -> &str {
        &self.message
    }

    /// True when the stored session is no longer accepted by the server.
    pub fn is_auth(&self) -> bool {
        self.kind == ApiErrorKind::Unauthorized || matches!(self.status, Some(401 | 403))
    }
}

impl fmt::Display for ApiError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.message)
    }
}

impl std::error::Error for ApiError {}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn http_status_is_readable_and_redacted() {
        let url = "http://user:pass@192.168.1.5:8096/emby/Items/42?api_key=s3cret";
        let err = ApiError::http_status(500, url);
        let message = err.to_string();
        assert!(message.contains("HTTP 500"), "{message}");
        assert!(
            message.contains("http://192.168.1.5:8096/emby/Items/42"),
            "redacted URL expected, got {message}"
        );
        assert!(!message.contains("s3cret"), "{message}");
        assert!(!message.contains("api_key"), "{message}");
        assert!(!message.contains("pass"), "{message}");
        assert_eq!(err.status(), Some(500));
        assert!(!err.is_auth());
    }

    #[test]
    fn unauthorized_status_is_detected() {
        let err = ApiError::http_status(401, "http://host/Users/u1/Items?api_key=secret");
        assert!(err.is_auth());
        assert_eq!(err.to_string(), "登录已失效，请重新登录");
        assert_eq!(err.status(), Some(401));

        let forbidden = ApiError::http_status(403, "http://host/System/Info");
        assert!(forbidden.is_auth());
        assert!(!forbidden.to_string().contains("api_key"));
    }

    #[test]
    fn transport_and_decode_errors_do_not_include_url_queries() {
        let err = ApiError::transport(format!(
            "无法连接 Emby 服务器：{}",
            redact_url("https://host/Videos/1/stream?static=true&api_key=abc")
        ));
        assert_eq!(err.kind(), ApiErrorKind::Transport);
        assert!(!err.to_string().contains("abc"));
        assert!(!err.to_string().contains("api_key"));
    }
}
