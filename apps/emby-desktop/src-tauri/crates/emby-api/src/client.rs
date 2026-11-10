//! Async Emby client.
//!
//! One shared [`reqwest::Client`] is passed into [`EmbyClient::new`] so all
//! commands reuse connections. Timeouts: 15 s for normal calls, 20 s for login.
//! Errors never contain the request body or the access token.

use std::fmt;
use std::time::Duration;

use reqwest::{Client, Method, RequestBuilder};
use serde_json::json;

use crate::auth::{authorization_header, CLIENT_NAME, CLIENT_VERSION};
use crate::error::{ApiError, ApiErrorKind, ApiResult};
use crate::model::{
    pick_playback_source, EmbyImageRequest, EmbyItem, EmbyItemsPage, EmbyItemsRequest,
    EmbyPlaybackRequest, EmbyPlaybackTarget, EmbyReportRequest, EmbyView, RawAuthResponse,
    RawItem, RawItemsResponse, RawPlaybackInfo, RawPublicSystemInfo, DEFAULT_IMAGE_WIDTH,
};
use crate::report::{ReportBody, ReportEvent};
use crate::ticks::ticks_to_seconds;
use crate::url::{absolutize, build_url, redact_url, with_api_key};

/// Timeout for normal API requests.
pub const DEFAULT_TIMEOUT: Duration = Duration::from_secs(15);
/// Timeout for `POST /Users/AuthenticateByName`.
pub const LOGIN_TIMEOUT: Duration = Duration::from_secs(20);

/// Fields requested for item lists/details. The frontend contract depends on
/// these (`userData`, `imageTags`, `primaryImageAspectRatio`, ...).
const ITEM_FIELDS: &str = "PrimaryImageAspectRatio,Overview,Genres,UserData";
/// Detail page: the list fields plus cast/crew, studios, taglines and the
/// streams (top level and per media source; servers differ).
const DETAIL_FIELDS: &str =
    "PrimaryImageAspectRatio,Overview,Genres,UserData,People,Studios,Taglines,MediaStreams,MediaSources";

/// Builds the shared HTTP client. `tls` is a crate feature, so the same call
/// works for plain-http tests and for https in the desktop build.
pub fn default_http_client() -> ApiResult<Client> {
    Client::builder()
        .timeout(DEFAULT_TIMEOUT)
        .user_agent(format!("{CLIENT_NAME}/{CLIENT_VERSION}"))
        .build()
        .map_err(|_| ApiError::config("无法初始化 HTTP 客户端"))
}

/// Result of a successful `AuthenticateByName` plus the resolved server name.
#[derive(Clone, PartialEq, Eq)]
pub struct LoginOutcome {
    pub access_token: String,
    pub user_id: String,
    pub user_name: String,
    pub server_id: Option<String>,
    pub server_name: String,
}

impl fmt::Debug for LoginOutcome {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("LoginOutcome")
            .field("access_token", &"<redacted>")
            .field("user_id", &self.user_id)
            .field("user_name", &self.user_name)
            .field("server_id", &self.server_id)
            .field("server_name", &self.server_name)
            .finish()
    }
}

/// Thin wrapper over Emby endpoints. Cloneable; the underlying `reqwest::Client`
/// is shared.
#[derive(Clone)]
pub struct EmbyClient {
    http: Client,
    server_url: String,
    device_id: String,
    device_name: String,
    token: Option<String>,
}

impl fmt::Debug for EmbyClient {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("EmbyClient")
            .field("server_url", &self.server_url)
            .field("device_id", &self.device_id)
            .field("device_name", &self.device_name)
            .field("token", &self.token.as_ref().map(|_| "<redacted>"))
            .finish()
    }
}

impl EmbyClient {
    /// Unauthenticated client (login, public system info).
    pub fn new(
        http: Client,
        server_url: impl Into<String>,
        device_id: impl Into<String>,
        device_name: impl Into<String>,
    ) -> Self {
        Self {
            http,
            server_url: server_url.into(),
            device_id: device_id.into(),
            device_name: device_name.into(),
            token: None,
        }
    }

    /// Authenticated client used for every request after `login`.
    pub fn with_token(
        http: Client,
        server_url: impl Into<String>,
        device_id: impl Into<String>,
        device_name: impl Into<String>,
        token: impl Into<String>,
    ) -> Self {
        Self {
            token: Some(token.into()),
            ..Self::new(http, server_url, device_id, device_name)
        }
    }

    pub fn server_url(&self) -> &str {
        &self.server_url
    }

    pub fn device_id(&self) -> &str {
        &self.device_id
    }

    /// Direct login. Username/password are only used for this request; the
    /// returned token is stored by the caller (never by this crate).
    pub async fn login(&self, username: &str, password: &str) -> ApiResult<LoginOutcome> {
        let username = username.trim();
        if username.is_empty() {
            return Err(ApiError::config("请输入用户名"));
        }

        let url = self.endpoint(&["Users", "AuthenticateByName"], &[])?;
        let payload = json!({ "Username": username, "Pw": password });
        let builder = self
            .request(Method::POST, &url)
            .timeout(LOGIN_TIMEOUT)
            .json(&payload);
        let body = self
            .send(builder, LOGIN_TIMEOUT, &url)
            .await
            .map_err(|err| match err.status() {
                // Wrong credentials are a user error, not an expired session.
                Some(401) | Some(403) => ApiError::new(
                    ApiErrorKind::Unauthorized,
                    "用户名或密码错误，或该用户没有登录权限",
                ),
                _ => err,
            })?;

        let auth: RawAuthResponse = parse_json(&body, "登录")?;
        let access_token = auth
            .access_token
            .as_deref()
            .map(str::trim)
            .filter(|token| !token.is_empty())
            .map(str::to_string)
            .ok_or_else(|| {
                ApiError::decode("该地址不是有效的 Emby 服务器（登录响应缺少 AccessToken）")
            })?;
        let user = auth.user.unwrap_or_default();
        let user_id = user
            .id
            .as_deref()
            .map(str::trim)
            .filter(|id| !id.is_empty())
            .map(str::to_string)
            .ok_or_else(|| {
                ApiError::decode("该地址不是有效的 Emby 服务器（登录响应缺少 User.Id）")
            })?;
        let user_name = user
            .name
            .as_deref()
            .map(str::trim)
            .filter(|name| !name.is_empty())
            .map(str::to_string)
            .unwrap_or_else(|| username.to_string());

        let server_name = auth
            .server_name
            .as_deref()
            .map(str::trim)
            .filter(|name| !name.is_empty())
            .map(str::to_string);

        let server_name = match server_name {
            Some(name) => name,
            // /System/Info/Public is the only reliable source for the display
            // name; failure is not fatal, the host name is a fine fallback.
            None => self
                .fetch_server_name()
                .await
                .unwrap_or_else(|| host_of(&self.server_url)),
        };

        Ok(LoginOutcome {
            access_token,
            user_id,
            user_name,
            server_id: auth.server_id.filter(|id| !id.trim().is_empty()),
            server_name,
        })
    }

    pub async fn views(&self, user_id: &str) -> ApiResult<Vec<EmbyView>> {
        let _token = self.token()?;
        let url = self.endpoint(&["Users", user_id, "Views"], &[])?;
        let body = self
            .send(self.request(Method::GET, &url), DEFAULT_TIMEOUT, &url)
            .await?;
        let raw: RawItemsResponse = parse_json(&body, "媒体库")?;
        Ok(raw
            .items
            .unwrap_or_default()
            .into_iter()
            .map(RawItem::into_view)
            .collect())
    }

    pub async fn items(
        &self,
        user_id: &str,
        req: &EmbyItemsRequest,
    ) -> ApiResult<EmbyItemsPage> {
        let _token = self.token()?;

        let mut query: Vec<(&str, String)> = vec![
            ("Fields", ITEM_FIELDS.to_string()),
            ("EnableUserData", "true".to_string()),
            ("EnableImageTypes", "Primary,Backdrop".to_string()),
            ("ImageTypeLimit", "1".to_string()),
            ("Recursive", req.recursive.unwrap_or(true).to_string()),
            (
                "SortBy",
                clean(req.sort_by.as_deref()).unwrap_or_else(|| "SortName".to_string()),
            ),
            ("SortOrder", "Ascending".to_string()),
        ];
        if let Some(parent) = clean(req.parent_id.as_deref()) {
            query.push(("ParentId", parent));
        }
        if let Some(types) = clean(req.include_item_types.as_deref()) {
            query.push(("IncludeItemTypes", types));
        }
        if let Some(term) = clean(req.search_term.as_deref()) {
            query.push(("SearchTerm", term));
        }
        if let Some(filters) = clean(req.filters.as_deref()) {
            query.push(("Filters", filters));
        }
        if let Some(start_index) = req.start_index {
            query.push(("StartIndex", start_index.to_string()));
        }
        if let Some(limit) = req.limit.filter(|limit| *limit > 0) {
            query.push(("Limit", limit.to_string()));
        }

        let pairs: Vec<(&str, &str)> = query
            .iter()
            .map(|(key, value)| (*key, value.as_str()))
            .collect();
        let url = self.endpoint(&["Users", user_id, "Items"], &pairs)?;
        let body = self
            .send(self.request(Method::GET, &url), DEFAULT_TIMEOUT, &url)
            .await?;
        let raw: RawItemsResponse = parse_json(&body, "媒体库条目列表")?;
        Ok(raw.into_page())
    }

    /// Continue-watching list (`GET /Users/{id}/Items/Resume`).
    ///
    /// Emby returns the most recently played item first; ordering is left to
    /// the server, so no `SortBy`/`SortOrder` is sent. Only video is requested
    /// and the response has the same shape as [`Self::items`]. `limit == 0`
    /// omits `Limit` entirely (server default page size).
    pub async fn resume(&self, user_id: &str, limit: u32) -> ApiResult<EmbyItemsPage> {
        let _token = self.token()?;

        let mut query: Vec<(&str, String)> = vec![
            ("MediaTypes", "Video".to_string()),
            ("EnableUserData", "true".to_string()),
            ("Fields", ITEM_FIELDS.to_string()),
            ("EnableImageTypes", "Primary,Backdrop".to_string()),
            ("ImageTypeLimit", "1".to_string()),
        ];
        if limit > 0 {
            query.push(("Limit", limit.to_string()));
        }

        let pairs: Vec<(&str, &str)> = query
            .iter()
            .map(|(key, value)| (*key, value.as_str()))
            .collect();
        let url = self.endpoint(&["Users", user_id, "Items", "Resume"], &pairs)?;
        let body = self
            .send(self.request(Method::GET, &url), DEFAULT_TIMEOUT, &url)
            .await?;
        let raw: RawItemsResponse = parse_json(&body, "继续观看列表")?;
        Ok(raw.into_page())
    }

    pub async fn item(&self, user_id: &str, item_id: &str) -> ApiResult<EmbyItem> {
        let _token = self.token()?;
        let item_id = required_item_id(item_id)?;
        let url = self.endpoint(
            &["Users", user_id, "Items", item_id],
            &[("Fields", DETAIL_FIELDS), ("EnableUserData", "true")],
        )?;
        let body = self
            .send(self.request(Method::GET, &url), DEFAULT_TIMEOUT, &url)
            .await?;
        let raw: RawItem = parse_json(&body, "条目")?;
        Ok(raw.into_item())
    }

    /// "More like this" (`GET /Items/{id}/Similar`).
    pub async fn similar(
        &self,
        user_id: &str,
        item_id: &str,
        limit: u32,
    ) -> ApiResult<EmbyItemsPage> {
        let _token = self.token()?;
        let item_id = required_item_id(item_id)?;
        let limit = limit.to_string();
        let url = self.endpoint(
            &["Items", item_id, "Similar"],
            &[
                ("UserId", user_id),
                ("Limit", limit.as_str()),
                ("Fields", ITEM_FIELDS),
                ("EnableUserData", "true"),
            ],
        )?;
        let body = self
            .send(self.request(Method::GET, &url), DEFAULT_TIMEOUT, &url)
            .await?;
        let raw: RawItemsResponse = parse_json(&body, "相似条目")?;
        Ok(raw.into_page())
    }

    /// Resolves a directly playable URL.
    ///
    /// 1. When `start_seconds` is not supplied, the item's saved position is
    ///    used as the resume point.
    /// 2. `PlaybackInfo` is requested with a `DeviceProfile` and transcoding
    ///    disabled.
    /// 3. The first source with a `DirectStreamUrl` wins; otherwise the
    ///    `/Videos/{id}/stream?static=true` fallback is built.
    /// 4. The URL is made absolute and gets `api_key` (the player does not
    ///    inject headers).
    pub async fn playback_target(
        &self,
        user_id: &str,
        req: &EmbyPlaybackRequest,
    ) -> ApiResult<EmbyPlaybackTarget> {
        let token = self.token()?.to_string();
        let item_id = required_item_id(&req.item_id)?.to_string();

        let start_seconds = match req.start_seconds.filter(|seconds| seconds.is_finite()) {
            Some(seconds) => seconds.max(0.0),
            None => {
                let item = self.item(user_id, &item_id).await?;
                item.user_data
                    .playback_position_ticks
                    .map(ticks_to_seconds)
                    .unwrap_or(0.0)
                    .max(0.0)
            }
        };

        let url = self.endpoint(&["Items", &item_id, "PlaybackInfo"], &[("UserId", user_id)])?;
        let payload = json!({
            "UserId": user_id,
            "StartTimeTicks": 0,
            "IsPlayback": true,
            "AutoOpenLiveStream": false,
            "EnableDirectPlay": true,
            "EnableDirectStream": true,
            "EnableTranscoding": false,
            // Without a profile Emby may transcode even when direct play would work.
            "DeviceProfile": device_profile(),
        });
        let body = self
            .send(
                self.request(Method::POST, &url).json(&payload),
                DEFAULT_TIMEOUT,
                &url,
            )
            .await?;
        let info: RawPlaybackInfo = parse_json(&body, "播放信息")?;

        let source = pick_playback_source(&info).ok_or_else(no_source_error)?;
        let source_id = source
            .id
            .as_deref()
            .map(str::trim)
            .filter(|id| !id.is_empty());
        let direct = source
            .direct_stream_url
            .as_deref()
            .map(str::trim)
            .filter(|url| !url.is_empty());

        let raw_url = match direct {
            Some(direct) => absolutize(&self.server_url, direct),
            None => {
                let source_id = source_id.ok_or_else(no_source_error)?;
                self.endpoint(
                    &["Videos", &item_id, "stream"],
                    &[("static", "true"), ("mediaSourceId", source_id)],
                )?
            }
        };
        let url = with_api_key(&raw_url, &token);

        let play_session_id = source
            .play_session_id
            .as_deref()
            .and_then(|id| clean(Some(id)))
            .or_else(|| {
                info.play_session_id
                    .as_deref()
                    .and_then(|id| clean(Some(id)))
            });

        Ok(EmbyPlaybackTarget {
            item_id,
            url,
            play_session_id,
            media_source_id: source_id.map(str::to_string),
            start_seconds,
        })
    }

    /// Reports one playback event. Mapping:
    /// - start -> `/Sessions/Playing` (no `EventName`)
    /// - progress/seek -> `/Sessions/Playing/Progress`, `TimeUpdate`
    /// - pause/unpause -> `/Sessions/Playing/Progress`, `Pause`/`Unpause`
    /// - stop/ended -> `/Sessions/Playing/Stopped`, `Stop`
    pub async fn report_playback(&self, req: &EmbyReportRequest) -> ApiResult<()> {
        let _token = self.token()?;
        required_item_id(&req.item_id)?;
        let event = ReportEvent::parse(&req.event)?;
        let body = ReportBody::new(req, event);
        let url = self.endpoint(event.segments(), &[])?;
        self.send(
            self.request(Method::POST, &url).json(&body),
            DEFAULT_TIMEOUT,
            &url,
        )
        .await?;
        Ok(())
    }

    /// Builds an absolute image URL with `api_key`. Defaults: `Primary`, 400 px.
    pub fn image_url(&self, req: &EmbyImageRequest) -> ApiResult<String> {
        let token = self.token()?.to_string();
        let item_id = required_item_id(&req.item_id)?;
        let image_type = req
            .image_type
            .as_deref()
            .map(str::trim)
            .filter(|kind| !kind.is_empty())
            .unwrap_or("Primary");
        let mut query: Vec<(&str, String)> = vec![(
            "maxWidth",
            req.max_width
                .unwrap_or(DEFAULT_IMAGE_WIDTH)
                .to_string(),
        )];
        if let Some(tag) = clean(req.tag.as_deref()) {
            query.push(("tag", tag));
        }
        let pairs: Vec<(&str, &str)> = query
            .iter()
            .map(|(key, value)| (*key, value.as_str()))
            .collect();
        let url = self.endpoint(&["Items", item_id, "Images", image_type], &pairs)?;
        Ok(with_api_key(&url, &token))
    }

    fn token(&self) -> ApiResult<&str> {
        self.token
            .as_deref()
            .map(str::trim)
            .filter(|token| !token.is_empty())
            .ok_or_else(|| ApiError::config("尚未登录 Emby 服务器"))
    }

    fn endpoint(&self, segments: &[&str], query: &[(&str, &str)]) -> ApiResult<String> {
        build_url(&self.server_url, segments, query)
    }

    fn request(&self, method: Method, url: &str) -> RequestBuilder {
        let authorization =
            authorization_header(&self.device_name, &self.device_id, self.token.as_deref());
        let mut builder = self
            .http
            .request(method, url)
            .header("X-Emby-Authorization", authorization)
            .header("Accept", "application/json");
        if let Some(token) = self.token.as_deref().map(str::trim).filter(|t| !t.is_empty()) {
            builder = builder.header("X-Emby-Token", token);
        }
        builder
    }

    async fn send(
        &self,
        builder: RequestBuilder,
        timeout: Duration,
        url: &str,
    ) -> ApiResult<Vec<u8>> {
        let response = builder
            .send()
            .await
            .map_err(|err| transport_error(err, timeout, url))?;
        let status = response.status();
        let body = response
            .bytes()
            .await
            .map_err(|err| transport_error(err, timeout, url))?;
        if !status.is_success() {
            return Err(ApiError::http_status(status.as_u16(), url));
        }
        Ok(body.to_vec())
    }

    async fn fetch_server_name(&self) -> Option<String> {
        let url = self.endpoint(&["System", "Info", "Public"], &[]).ok()?;
        let body = self
            .send(self.request(Method::GET, &url), DEFAULT_TIMEOUT, &url)
            .await
            .ok()?;
        let info: RawPublicSystemInfo = serde_json::from_slice(&body).ok()?;
        info.server_name
            .map(|name| name.trim().to_string())
            .filter(|name| !name.is_empty())
    }
}

fn required_item_id(item_id: &str) -> ApiResult<&str> {
    let item_id = item_id.trim();
    if item_id.is_empty() {
        return Err(ApiError::config("缺少条目 id"));
    }
    Ok(item_id)
}

fn no_source_error() -> ApiError {
    ApiError::no_playable_source("该媒体没有可直接播放的源（需要转码，当前版本尚未实现）")
}

fn clean(value: Option<&str>) -> Option<String> {
    value
        .map(str::trim)
        .filter(|value| !value.is_empty())
        .map(str::to_string)
}

fn parse_json<T: serde::de::DeserializeOwned>(body: &[u8], what: &str) -> ApiResult<T> {
    serde_json::from_slice(body).map_err(|_| {
        ApiError::decode(format!(
            "Emby {what}响应无法解析（该地址可能不是 Emby 服务器，或版本过旧）"
        ))
    })
}

/// Maps transport failures without ever formatting the underlying reqwest error:
/// its `Display` includes the URL, which may contain `api_key`.
fn transport_error(err: reqwest::Error, timeout: Duration, url: &str) -> ApiError {
    let target = redact_url(url);
    if err.is_timeout() {
        ApiError::transport(format!(
            "连接 Emby 服务器超时（{} 秒）：{target}",
            timeout.as_secs()
        ))
    } else if err.is_connect() {
        ApiError::transport(format!(
            "无法连接 Emby 服务器，请检查地址与网络：{target}"
        ))
    } else if err.is_decode() {
        ApiError::decode(format!("Emby 响应无法解析：{target}"))
    } else {
        ApiError::transport(format!("Emby 网络请求失败：{target}"))
    }
}

fn host_of(server_url: &str) -> String {
    url::Url::parse(server_url)
        .ok()
        .and_then(|url| {
            url.host_str().map(|host| match url.port() {
                Some(port) => format!("{host}:{port}"),
                None => host.to_string(),
            })
        })
        .unwrap_or_else(|| "Emby".to_string())
}

/// Permissive `DeviceProfile` for direct play: mpv handles a wide range of
/// containers/codecs, so codecs are left unrestricted and transcoding is
/// disabled in the PlaybackInfo body.
fn device_profile() -> serde_json::Value {
    json!({
        "Name": CLIENT_NAME,
        "MaxStreamingBitrate": 140_000_000,
        "DirectPlayProfiles": [
            {
                "Container": "mkv,mp4,m4v,mov,webm,avi,ts,m2ts,flv,wmv,mpg,mpeg,ogv,3gp",
                "Type": "Video"
            },
            {
                "Container": "mp3,flac,aac,m4a,ogg,opus,wav,wma",
                "Type": "Audio"
            }
        ],
        "TranscodingProfiles": []
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Serves exactly one HTTP request and returns the raw request text.
    /// The test thread drives reqwest while this thread only blocks on the
    /// loopback socket, so no extra runtime feature is needed.
    fn spawn_mock_server(body: &'static str) -> (String, std::thread::JoinHandle<String>) {
        use std::io::{Read, Write};
        use std::net::TcpListener;

        let listener = TcpListener::bind("127.0.0.1:0").expect("bind mock server");
        let addr = listener.local_addr().expect("mock server address");
        let handle = std::thread::spawn(move || {
            let (mut stream, _) = listener.accept().expect("accept request");
            stream
                .set_read_timeout(Some(Duration::from_secs(5)))
                .expect("read timeout");

            let mut request = Vec::new();
            let mut chunk = [0_u8; 1024];
            loop {
                let read = stream.read(&mut chunk).expect("read request");
                if read == 0 {
                    break;
                }
                request.extend_from_slice(&chunk[..read]);
                if request.windows(4).any(|window| window == b"\r\n\r\n") {
                    break;
                }
            }

            let response = format!(
                "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}",
                body.len(),
                body
            );
            stream
                .write_all(response.as_bytes())
                .expect("write response");
            String::from_utf8(request).expect("utf-8 request")
        });
        (format!("http://{addr}"), handle)
    }

    fn test_client(server_url: String) -> EmbyClient {
        EmbyClient::with_token(
            default_http_client().expect("http client"),
            server_url,
            "test-device",
            "Test",
            "test-token",
        )
    }

    #[tokio::test]
    async fn resume_uses_the_official_endpoint_without_a_client_side_sort() {
        const PAGE: &str = r#"{
            "Items": [
                {
                    "Id": "newest",
                    "Name": "Newest episode",
                    "Type": "Episode",
                    "MediaType": "Video",
                    "UserData": {"PlaybackPositionTicks": 90000000, "Played": false}
                },
                {
                    "Id": "older",
                    "Name": "Older movie",
                    "Type": "Movie",
                    "MediaType": "Video",
                    "UserData": {"PlaybackPositionTicks": 10000000, "Played": false}
                }
            ],
            "TotalRecordCount": 2,
            "StartIndex": 0
        }"#;
        let (server_url, server) = spawn_mock_server(PAGE);

        let page = test_client(server_url)
            .resume("u1", 12)
            .await
            .expect("resume request");
        let request = server.join().expect("mock server thread");

        let request_line = request.lines().next().unwrap_or_default();
        assert!(
            request_line.starts_with("GET /Users/u1/Items/Resume?"),
            "{request_line}"
        );
        assert!(request_line.contains("MediaTypes=Video"), "{request_line}");
        assert!(request_line.contains("EnableUserData=true"), "{request_line}");
        assert!(request_line.contains("Fields=PrimaryImageAspectRatio"), "{request_line}");
        assert!(
            request_line.contains("EnableImageTypes=Primary%2CBackdrop")
                || request_line.contains("EnableImageTypes=Primary,Backdrop"),
            "{request_line}"
        );
        assert!(request_line.contains("ImageTypeLimit=1"), "{request_line}");
        assert!(request_line.contains("Limit=12"), "{request_line}");
        assert!(!request_line.contains("SortBy"), "{request_line}");
        assert!(!request_line.contains("SortOrder"), "{request_line}");
        assert!(!request_line.contains("Filters="), "{request_line}");

        assert_eq!(page.total_record_count, 2);
        assert_eq!(page.start_index, 0);
        assert_eq!(page.items.len(), 2);
        // Server order is preserved verbatim; the client must not re-sort.
        assert_eq!(page.items[0].id, "newest");
        assert_eq!(page.items[1].id, "older");
        assert_eq!(
            page.items[0].user_data.playback_position_ticks,
            Some(90_000_000)
        );
    }

    #[tokio::test]
    async fn resume_with_zero_limit_omits_limit() {
        const PAGE: &str = r#"{"Items":[],"TotalRecordCount":0,"StartIndex":0}"#;
        let (server_url, server) = spawn_mock_server(PAGE);

        let page = test_client(server_url)
            .resume("u1", 0)
            .await
            .expect("resume request");
        let request = server.join().expect("mock server thread");

        let request_line = request.lines().next().unwrap_or_default();
        assert!(
            request_line.starts_with("GET /Users/u1/Items/Resume?"),
            "{request_line}"
        );
        assert!(
            !request_line
                .split('?')
                .nth(1)
                .unwrap_or_default()
                .split('&')
                .any(|pair| pair.starts_with("Limit=")),
            "{request_line}"
        );
        assert!(page.items.is_empty());
    }

    #[test]
    fn device_profile_keeps_direct_play_enabled_for_video() {
        let profile = device_profile();
        let profiles = profile["DirectPlayProfiles"].as_array().unwrap();
        assert!(profiles.iter().any(|entry| {
            entry["Type"] == "Video" && entry["Container"].as_str().unwrap().contains("mkv")
        }));
        assert!(profiles
            .iter()
            .any(|entry| entry["Type"] == "Audio"));
        assert_eq!(profile["TranscodingProfiles"], json!([]));
    }

    #[test]
    fn host_of_handles_port_and_invalid_urls() {
        assert_eq!(host_of("http://192.168.1.5:8096"), "192.168.1.5:8096");
        assert_eq!(host_of("https://example.com/emby"), "example.com");
        assert_eq!(host_of("not a url"), "Emby");
    }

    #[test]
    fn missing_item_id_is_rejected_before_the_request() {
        assert!(required_item_id("   ").is_err());
        assert_eq!(required_item_id(" i1 ").unwrap(), "i1");
    }

    #[test]
    fn no_source_error_mentions_transcoding() {
        let error = no_source_error();
        assert!(error.to_string().contains("转码"), "{error}");
        assert_eq!(
            error.kind(),
            ApiErrorKind::NoPlayableSource
        );
    }

    #[tokio::test]
    async fn item_requests_the_detail_fields() {
        const ITEM: &str = r#"{"Id": "m1", "Name": "Movie", "Type": "Movie", "People": [{"Name": "A", "Type": "Actor"}]}"#;
        let (server_url, server) = spawn_mock_server(ITEM);
        let item = test_client(server_url)
            .item("u1", "m1")
            .await
            .expect("item request");
        let request = server.join().expect("mock server thread");
        let request_line = request.lines().next().unwrap_or_default();
        assert!(request_line.starts_with("GET /Users/u1/Items/m1?"), "{request_line}");
        for field in ["People", "Studios", "Taglines", "MediaStreams", "MediaSources"] {
            assert!(request_line.contains(field), "missing {field}: {request_line}");
        }
        assert_eq!(item.people.len(), 1);
    }

    #[tokio::test]
    async fn similar_uses_the_item_endpoint_with_the_user() {
        const PAGE: &str = r#"{"Items": [{"Id": "m2", "Name": "Other", "Type": "Movie"}], "TotalRecordCount": 1, "StartIndex": 0}"#;
        let (server_url, server) = spawn_mock_server(PAGE);
        let page = test_client(server_url)
            .similar("u1", "m1", 12)
            .await
            .expect("similar request");
        let request = server.join().expect("mock server thread");
        let request_line = request.lines().next().unwrap_or_default();
        assert!(request_line.starts_with("GET /Items/m1/Similar?"), "{request_line}");
        assert!(request_line.contains("UserId=u1"), "{request_line}");
        assert!(request_line.contains("Limit=12"), "{request_line}");
        assert_eq!(page.items.len(), 1);
        assert_eq!(page.items[0].id, "m2");
    }

    #[test]
    fn item_fields_cover_the_frontend_contract() {
        for field in ["PrimaryImageAspectRatio", "Overview", "Genres", "UserData"] {
            assert!(ITEM_FIELDS.contains(field), "missing {field}");
        }
    }
}
