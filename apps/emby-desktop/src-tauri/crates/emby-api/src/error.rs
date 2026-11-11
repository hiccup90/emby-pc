//! Error type shared by every `emby-api` call.
//!
//! Every failure carries an explicit semantic kind, so callers (the Tauri
//! command layer in particular) never have to re-inspect an HTTP status: 401
//! is [`ApiErrorKind::TokenExpired`], 403 is [`ApiErrorKind::Forbidden`], and
//! a permission problem is never reported as an expired session.
//!
//! Messages are meant to be shown in the UI: short, Chinese and free of
//! secrets. URLs included in messages are passed through [`redact_url`], which
//! removes the query string (where `api_key` lives) and URL credentials.

use std::fmt;

use crate::url::redact_url;

/// Semantic classification of an API failure.
///
/// This is the only thing callers should branch on; the HTTP status is already
/// folded into the kind (and into the message for diagnostics).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ApiErrorKind {
    /// HTTP 401: the token is no longer accepted and the session must be dropped.
    TokenExpired,
    /// HTTP 403: authenticated, but this user may not perform the operation.
    /// Deliberately *not* treated as an expired session.
    Forbidden,
    /// `AuthenticateByName` rejected the username/password (401/403 on login).
    /// Distinct from [`ApiErrorKind::TokenExpired`]: there is no session yet.
    InvalidCredentials,
    /// Bad caller input: empty id, invalid server URL, unknown event name.
    InvalidInput,
    /// Local persistence failed (session/device file/directory).
    Storage,
    /// Connection failure, timeout, DNS/TLS or a broken response body.
    Network,
    /// HTTP 5xx or a response the client cannot act on.
    Server,
    /// HTTP 404.
    NotFound,
    /// The response was not valid Emby JSON.
    Decode,
    /// PlaybackInfo returned no source the client can play without transcoding.
    NoPlayableSource,
}

/// Failure returned by every fallible `emby-api` operation.
#[derive(Debug, Clone)]
pub struct ApiError {
    kind: ApiErrorKind,
    message: String,
}

/// Convenience alias used across the crate.
pub type ApiResult<T> = Result<T, ApiError>;

impl ApiError {
    pub fn new(kind: ApiErrorKind, message: impl Into<String>) -> Self {
        Self {
            kind,
            message: message.into(),
        }
    }

    pub fn invalid_input(message: impl Into<String>) -> Self {
        Self::new(ApiErrorKind::InvalidInput, message)
    }

    pub fn storage(message: impl Into<String>) -> Self {
        Self::new(ApiErrorKind::Storage, message)
    }

    pub fn network(message: impl Into<String>) -> Self {
        Self::new(ApiErrorKind::Network, message)
    }

    pub fn invalid_credentials(message: impl Into<String>) -> Self {
        Self::new(ApiErrorKind::InvalidCredentials, message)
    }

    pub fn decode(message: impl Into<String>) -> Self {
        Self::new(ApiErrorKind::Decode, message)
    }

    pub fn no_playable_source(message: impl Into<String>) -> Self {
        Self::new(ApiErrorKind::NoPlayableSource, message)
    }

    /// Maps a non-success HTTP status to a readable, semantic error.
    ///
    /// `url` is only ever embedded after [`redact_url`], so a token that leaked
    /// into a query string cannot reach the frontend or a log line.
    pub fn http_status(status: u16, url: &str) -> Self {
        let target = redact_url(url);
        match status {
            401 => Self::new(ApiErrorKind::TokenExpired, "登录已失效，请重新登录"),
            403 => Self::new(
                ApiErrorKind::Forbidden,
                format!("没有权限执行该操作（HTTP 403）：{target}"),
            ),
            404 => Self::new(
                ApiErrorKind::NotFound,
                format!("找不到请求的 Emby 资源（HTTP 404）：{target}"),
            ),
            500..=599 => Self::new(
                ApiErrorKind::Server,
                format!("Emby 服务器出错（HTTP {status}）：{target}"),
            ),
            _ => Self::new(
                ApiErrorKind::Server,
                format!("Emby 请求失败（HTTP {status}）：{target}"),
            ),
        }
    }

    pub fn kind(&self) -> ApiErrorKind {
        self.kind
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
        assert_eq!(err.kind(), ApiErrorKind::Server);
    }

    #[test]
    fn statuses_map_to_distinct_semantic_kinds() {
        let expired = ApiError::http_status(401, "http://host/Users/u1?api_key=secret");
        assert_eq!(expired.kind(), ApiErrorKind::TokenExpired);
        assert_eq!(expired.to_string(), "登录已失效，请重新登录");

        // 403 is a permission problem; it must never be reported as an expired
        // session, which is what made the UI drop a valid login.
        let forbidden = ApiError::http_status(403, "http://host/System/Info?api_key=secret");
        assert_eq!(forbidden.kind(), ApiErrorKind::Forbidden);
        assert!(!forbidden.to_string().contains("api_key"));
        assert!(!forbidden.to_string().contains("登录已失效"));

        assert_eq!(
            ApiError::http_status(404, "http://host/Items/1").kind(),
            ApiErrorKind::NotFound
        );
        assert_eq!(
            ApiError::http_status(400, "http://host/Items/1").kind(),
            ApiErrorKind::Server
        );
        assert_eq!(
            ApiError::http_status(503, "http://host/Items/1").kind(),
            ApiErrorKind::Server
        );
    }

    #[test]
    fn network_and_decode_errors_do_not_include_url_queries() {
        let err = ApiError::network(format!(
            "无法连接 Emby 服务器：{}",
            redact_url("https://host/Videos/1/stream?static=true&api_key=abc")
        ));
        assert_eq!(err.kind(), ApiErrorKind::Network);
        assert!(!err.to_string().contains("abc"));
        assert!(!err.to_string().contains("api_key"));

        let decode = ApiError::decode("bad json");
        assert_eq!(decode.kind(), ApiErrorKind::Decode);
        assert_eq!(ApiError::storage("disk").kind(), ApiErrorKind::Storage);
        assert_eq!(
            ApiError::invalid_input("empty id").kind(),
            ApiErrorKind::InvalidInput
        );
        assert_eq!(
            ApiError::invalid_credentials("bad password").kind(),
            ApiErrorKind::InvalidCredentials
        );
    }
}
