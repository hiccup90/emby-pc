//! Emby HTTP API client used by the Emby Desktop Tauri backend.
//!
//! Design constraints (see the module docs of each item):
//! - This crate never depends on Tauri. The app passes the config directory in
//!   and turns [`ApiError`] into command errors itself.
//! - Every call is `async` and uses one shared [`reqwest::Client`] supplied by
//!   the caller, so the UI thread is never blocked by I/O.
//! - Access tokens are only sent in headers or in the `api_key` query parameter
//!   of playback/image URLs. They are never written to logs or error messages:
//!   [`redact_url`] strips query strings and credentials, and the session type
//!   has a redacting [`std::fmt::Debug`] implementation.
//! - Response DTOs mirror the Emby (PascalCase) field names; the types returned
//!   to the frontend serialize as camelCase as required by the TypeScript
//!   contract.
//!
//! `PlaybackInfo` always carries a `DeviceProfile`; without one Emby applies a
//! default profile that may decide to transcode even when the client could play
//! the file directly.

mod auth;
mod client;
mod error;
mod model;
mod report;
mod session;
mod ticks;
pub mod url;

pub use auth::{authorization_header, default_device_name, CLIENT_NAME, CLIENT_VERSION};
pub use client::{default_http_client, EmbyClient, LoginOutcome, DEFAULT_TIMEOUT, LOGIN_TIMEOUT};
pub use error::{ApiError, ApiErrorKind, ApiResult};
pub use model::{
    EmbyImageRequest, EmbyItem, EmbyItemsPage, EmbyItemsRequest, EmbyLoginRequest,
    EmbyPlaybackRequest, EmbyPlaybackTarget, EmbyReportRequest, EmbySession, EmbyUserData,
    EmbyView, DEFAULT_IMAGE_WIDTH,
};
pub use report::ReportEvent;
pub use session::{generate_device_id, SessionStore, StoredSession};
pub use ticks::{seconds_to_ticks, ticks_to_seconds, TICKS_PER_SECOND};
pub use url::{build_url, normalize_server_url, redact_url, with_api_key};
