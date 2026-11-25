//! Async Emby client.
//!
//! One shared [`reqwest::Client`] is passed into [`EmbyClient::new`] so all
//! commands reuse connections. Timeouts: 15 s for normal calls, 20 s for login,
//! 5 s for logout (a slow server must not delay a local sign-out).
//! Errors never contain the request body or the access token.

use std::fmt;
use std::time::Duration;

use bytes::Bytes;
use reqwest::{Client, Method, RequestBuilder};
use serde_json::json;

use crate::auth::{authorization_header, CLIENT_NAME, CLIENT_VERSION};
use crate::clean;
use crate::error::{ApiError, ApiErrorKind, ApiResult};
use crate::model::{
    chapters_from_raw, pick_playback_source, EmbyApplyRemoteSearchRequest, EmbyChapter,
    EmbyImageRequest, EmbyItem, EmbyItemsPage, EmbyItemsRequest, EmbyPlaybackRequest,
    EmbyPlaybackTarget, EmbyRemoteSearchRequest, EmbyReportRequest, EmbyView, RawAuthResponse,
    RawChapter, RawItem, RawItemsResponse, RawPlaybackInfo, RawPublicSystemInfo,
    DEFAULT_IMAGE_WIDTH,
};
use crate::report::{ReportBody, ReportEvent};
use crate::ticks::ticks_to_seconds;
use crate::url::{absolutize, build_url, redact_url, with_api_key};

/// Timeout for normal API requests.
pub const DEFAULT_TIMEOUT: Duration = Duration::from_secs(15);
/// Timeout for `POST /Users/AuthenticateByName`.
pub const LOGIN_TIMEOUT: Duration = Duration::from_secs(20);
/// Timeout for `POST /Sessions/Logout`: revoking the token must never keep the
/// UI waiting on a slow or unreachable server; the local session is dropped
/// regardless.
pub const LOGOUT_TIMEOUT: Duration = Duration::from_secs(5);

/// Fields requested for item lists/details. The frontend contract depends on
/// these (`userData`, `imageTags`, `primaryImageAspectRatio`, ...).
const ITEM_FIELDS: &str =
    "PrimaryImageAspectRatio,Overview,Genres,UserData,DateCreated";
/// Detail page: the list fields plus cast/crew, studios, taglines and the
/// streams (top level and per media source; servers differ).
const DETAIL_FIELDS: &str =
    "PrimaryImageAspectRatio,Overview,Genres,UserData,DateCreated,People,Studios,Taglines,MediaStreams,MediaSources,Chapters";

/// Builds the shared HTTP client. `tls` is a crate feature, so the same call
/// works for plain-http tests and for https in the desktop build.
pub fn default_http_client() -> ApiResult<Client> {
    Client::builder()
        .timeout(DEFAULT_TIMEOUT)
        .user_agent(format!("{CLIENT_NAME}/{CLIENT_VERSION}"))
        .build()
        .map_err(|_| ApiError::invalid_input("无法初始化 HTTP 客户端"))
}

/// Result of a successful `AuthenticateByName` plus the resolved server name.
#[derive(Clone, PartialEq, Eq)]
pub struct LoginOutcome {
    pub access_token: String,
    pub user_id: String,
    pub user_name: String,
    pub server_name: String,
}

impl fmt::Debug for LoginOutcome {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("LoginOutcome")
            .field("access_token", &"<redacted>")
            .field("user_id", &self.user_id)
            .field("user_name", &self.user_name)
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

    /// Direct login. Username/password are only used for this request; the
    /// returned token is stored by the caller (never by this crate).
    pub async fn login(&self, username: &str, password: &str) -> ApiResult<LoginOutcome> {
        let username = username.trim();
        if username.is_empty() {
            return Err(ApiError::invalid_input("请输入用户名"));
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
            .map_err(|err| match err.kind() {
                // Wrong credentials are a user error, not an expired session.
                ApiErrorKind::TokenExpired | ApiErrorKind::Forbidden => {
                    ApiError::invalid_credentials("用户名或密码错误，或该用户没有登录权限")
                }
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
            server_name,
        })
    }

    /// Checks that the stored token is still accepted for `user_id`.
    ///
    /// `GET /Users/{id}` is the only endpoint that both requires a valid token
    /// and is available to a normal user (Emby has no `/Users/Me`). Errors keep
    /// their semantic kind, so the caller can tell an expired token
    /// ([`ApiErrorKind::TokenExpired`]) from a permission problem
    /// ([`ApiErrorKind::Forbidden`]) or an unknown user
    /// ([`ApiErrorKind::NotFound`]) without looking at HTTP statuses.
    pub async fn current_user(&self, user_id: &str) -> ApiResult<()> {
        let _token = self.token()?;
        let user_id = required_user_id(user_id)?;
        let url = self.endpoint(&["Users", user_id], &[])?;
        self.send(self.request(Method::GET, &url), DEFAULT_TIMEOUT, &url)
            .await
            .map(|_body| ())
    }

    /// Revokes the access token server-side (`POST /Sessions/Logout`).
    ///
    /// Uses a short timeout: the local session is dropped by the caller no
    /// matter what this returns, so a slow server must not block signing out.
    pub async fn logout(&self) -> ApiResult<()> {
        let _token = self.token()?;
        let url = self.endpoint(&["Sessions", "Logout"], &[])?;
        self.send(
            self.request(Method::POST, &url),
            LOGOUT_TIMEOUT,
            &url,
        )
        .await
        .map(|_body| ())
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
            // Thumb is the image the thumbnail view prefers. Without asking
            // for it Emby leaves ImageTags.Thumb out of the response, so every
            // landscape card had to fall back to a poster.
            ("EnableImageTypes", "Primary,Thumb,Backdrop".to_string()),
            ("ImageTypeLimit", "1".to_string()),
            ("Recursive", req.recursive.unwrap_or(true).to_string()),
            (
                "SortBy",
                clean(req.sort_by.as_deref()).unwrap_or_else(|| "SortName".to_string()),
            ),
            ("SortOrder", sort_order(req.sort_order.as_deref()).to_string()),
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
        if let Some(genres) = clean(req.genres.as_deref()) {
            query.push(("Genres", genres));
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
            // The hero and continue cards are landscape: ask for Thumb too,
            // otherwise an episode without a Backdrop falls back to its poster.
            ("EnableImageTypes", "Primary,Thumb,Backdrop".to_string()),
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

    /// Marks an item watched (`POST`) or unwatched (`DELETE`) for the user.
    pub async fn set_played(&self, user_id: &str, item_id: &str, played: bool) -> ApiResult<()> {
        let _token = self.token()?;
        let item_id = required_item_id(item_id)?;
        let method = if played { Method::POST } else { Method::DELETE };
        let url = self.endpoint(&["Users", user_id, "PlayedItems", item_id], &[])?;
        self.send(self.request(method, &url), DEFAULT_TIMEOUT, &url)
            .await?;
        Ok(())
    }

    /// Adds (`POST`) or removes (`DELETE`) the user's favorite mark.
    pub async fn set_favorite(
        &self,
        user_id: &str,
        item_id: &str,
        favorite: bool,
    ) -> ApiResult<()> {
        let _token = self.token()?;
        let item_id = required_item_id(item_id)?;
        let method = if favorite {
            Method::POST
        } else {
            Method::DELETE
        };
        let url = self.endpoint(&["Users", user_id, "FavoriteItems", item_id], &[])?;
        self.send(self.request(method, &url), DEFAULT_TIMEOUT, &url)
            .await?;
        Ok(())
    }

    /// Deletes one or more items (`POST /Items/Delete?Ids=...`).
    ///
    /// Emby deletes the library entry and, for library content, its media
    /// files; the caller must show a confirmation first. The server enforces
    /// the administrator permission.
    pub async fn delete_items(&self, ids: &[String]) -> ApiResult<()> {
        let _token = self.token()?;
        let ids: Vec<&str> = ids
            .iter()
            .map(|id| id.trim())
            .filter(|id| !id.is_empty())
            .collect();
        if ids.is_empty() {
            return Err(ApiError::invalid_input("缺少要删除的条目 id"));
        }
        let joined = ids.join(",");
        let url = self.endpoint(&["Items", "Delete"], &[("Ids", joined.as_str())])?;
        self.send(self.request(Method::POST, &url), DEFAULT_TIMEOUT, &url)
            .await?;
        Ok(())
    }

    /// File paths Emby will delete with the item (`GET /Items/{id}/DeleteInfo`).
    /// Used only to make the destructive confirmation explicit.
    pub async fn delete_info(&self, item_id: &str) -> ApiResult<Vec<String>> {
        let _token = self.token()?;
        let item_id = required_item_id(item_id)?;
        let url = self.endpoint(&["Items", item_id, "DeleteInfo"], &[])?;
        let body = self
            .send(self.request(Method::GET, &url), DEFAULT_TIMEOUT, &url)
            .await?;
        let value: serde_json::Value = parse_json(&body, "删除信息")?;
        Ok(value
            .get("Paths")
            .and_then(serde_json::Value::as_array)
            .map(|paths| {
                paths
                    .iter()
                    .filter_map(serde_json::Value::as_str)
                    .map(str::to_string)
                    .collect()
            })
            .unwrap_or_default())
    }

    /// Searches the enabled metadata providers (`POST /Items/RemoteSearch/{Type}`).
    ///
    /// The returned objects are the provider results exactly as Emby sends
    /// them; the frontend passes the chosen object back unchanged to
    /// [`Self::apply_remote_search`].
    pub async fn remote_search(
        &self,
        req: &EmbyRemoteSearchRequest,
    ) -> ApiResult<Vec<serde_json::Value>> {
        let _token = self.token()?;
        let item_id = required_item_id(&req.item_id)?;
        let item_type = req.item_type.trim();
        if item_type.is_empty() {
            return Err(ApiError::invalid_input("缺少条目类型"));
        }
        let name = req.name.trim();
        if name.is_empty() {
            return Err(ApiError::invalid_input("请输入要搜索的名称"));
        }
        // Official Identify sends the text fields from the dialog's
        // `data-lookup` attributes; the year key is `Year`, not
        // `ProductionYear` (that is the response/card field).
        let mut search_info = json!({ "ProviderIds": {}, "Name": name });
        if let Some(year) = req.production_year.filter(|year| *year > 0) {
            search_info["Year"] = json!(year);
        }
        let body = json!({ "SearchInfo": search_info, "ItemId": item_id });
        let url = self.endpoint(&["Items", "RemoteSearch", item_type], &[])?;
        let response = self
            .send(
                self.request(Method::POST, &url).json(&body),
                DEFAULT_TIMEOUT,
                &url,
            )
            .await?;
        parse_json(&response, "远程搜索结果")
    }

    /// Applies a chosen remote-search result (`POST /Items/RemoteSearch/Apply/{id}`).
    pub async fn apply_remote_search(&self, req: &EmbyApplyRemoteSearchRequest) -> ApiResult<()> {
        let _token = self.token()?;
        let item_id = required_item_id(&req.item_id)?;
        if req.result.is_null() {
            return Err(ApiError::invalid_input("请选择识别结果"));
        }
        let replace = if req.replace_all_images {
            "true"
        } else {
            "false"
        };
        let url = self.endpoint(
            &["Items", "RemoteSearch", "Apply", item_id],
            &[("ReplaceAllImages", replace)],
        )?;
        self.send(
            self.request(Method::POST, &url).json(&req.result),
            DEFAULT_TIMEOUT,
            &url,
        )
        .await?;
        Ok(())
    }

    /// Absolute URL for one remote-search result image. Emby serves provider
    /// images through `Items/RemoteSearch/Image`; the raw result only carries
    /// a provider-relative `ImageUrl`.
    pub fn remote_search_image_url(
        &self,
        image_url: &str,
        provider: Option<&str>,
    ) -> ApiResult<String> {
        let token = self.token()?.to_string();
        let image_url = image_url.trim();
        if image_url.is_empty() {
            return Err(ApiError::invalid_input("缺少识别结果的图片地址"));
        }
        let mut query: Vec<(&str, String)> = vec![("imageUrl", image_url.to_string())];
        if let Some(provider) = clean(provider) {
            query.push(("ProviderName", provider));
        }
        let pairs: Vec<(&str, &str)> = query
            .iter()
            .map(|(key, value)| (*key, value.as_str()))
            .collect();
        let url = self.endpoint(&["Items", "RemoteSearch", "Image"], &pairs)?;
        Ok(with_api_key(&url, &token))
    }

    /// Works featuring a person (`GET /Persons/{id}/Items`).
    ///
    /// The response uses the regular item shape, so it goes through the same
    /// `RawItemsResponse` parser as `items`. `limit == 0` omits `Limit`
    /// entirely (server default page size).
    pub async fn person_items(
        &self,
        user_id: &str,
        person_id: &str,
        limit: u32,
    ) -> ApiResult<EmbyItemsPage> {
        let _token = self.token()?;
        let person_id = required_person_id(person_id)?;

        let mut query: Vec<(&str, String)> = vec![
            ("UserId", user_id.to_string()),
            ("Recursive", "true".to_string()),
            ("Fields", ITEM_FIELDS.to_string()),
            ("EnableUserData", "true".to_string()),
            ("EnableImageTypes", "Primary,Backdrop".to_string()),
            ("ImageTypeLimit", "1".to_string()),
            ("SortBy", "SortName".to_string()),
            ("SortOrder", "Ascending".to_string()),
        ];
        if limit > 0 {
            query.push(("Limit", limit.to_string()));
        }

        let pairs: Vec<(&str, &str)> = query
            .iter()
            .map(|(key, value)| (*key, value.as_str()))
            .collect();
        let url = self.endpoint(&["Persons", person_id, "Items"], &pairs)?;
        let body = self
            .send(self.request(Method::GET, &url), DEFAULT_TIMEOUT, &url)
            .await?;
        let raw: RawItemsResponse = parse_json(&body, "演员作品列表")?;
        Ok(raw.into_page())
    }

    /// Resolves a directly playable URL.
    ///
    /// 1. The item is read first: it carries the saved position and, before
    ///    `PlaybackInfo` can replace them, the chapters (see step 4).
    /// 2. `PlaybackInfo` is requested with a `DeviceProfile` and transcoding
    ///    disabled.
    /// 3. The first source with a `DirectStreamUrl` wins; otherwise the
    ///    `/Videos/{id}/stream?static=true` fallback is built.
    /// 4. Chapters come from the media source, or from the item read when the
    ///    source list lost its intro markers (`playback_chapters`). The URL is
    ///    made absolute and gets `api_key` (the player does not inject headers).
    pub async fn playback_target(
        &self,
        user_id: &str,
        req: &EmbyPlaybackRequest,
    ) -> ApiResult<EmbyPlaybackTarget> {
        let token = self.token()?.to_string();
        let item_id = required_item_id(&req.item_id)?.to_string();

        // Read the item before `PlaybackInfo`. Emby probes a strm media source
        // the first time it is played — the `PlaybackInfo` call below is what
        // triggers it — and saves the probe result over the item's chapters, so
        // that one launch comes back without the `IntroStart`/`IntroEnd`
        // markers. The item read is a database read and still has them.
        let item = self.item(user_id, &item_id).await?;

        let start_seconds = match req.start_seconds.filter(|seconds| seconds.is_finite()) {
            Some(seconds) => seconds.max(0.0),
            None => item
                .user_data
                .playback_position_ticks
                .map(ticks_to_seconds)
                .unwrap_or(0.0)
                .max(0.0),
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
            // The source is borrowed from `info`; chapters are tiny and this is
            // one launch per playback, so cloning them is cheaper than adding a
            // second representation just to move them out.
            chapters: playback_chapters(source.chapters.clone(), item.chapters),
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
            .ok_or_else(|| ApiError::invalid_input("尚未登录 Emby 服务器"))
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
    ) -> ApiResult<Bytes> {
        let response = builder
            .send()
            .await
            .map_err(|err| transport_error(err, timeout, url))?;
        let status = response.status();
        if !status.is_success() {
            // Do not download an error page just to discard it. The status is
            // all the caller maps on; the body is not part of the contract.
            return Err(ApiError::http_status(status.as_u16(), url));
        }
        response
            .bytes()
            .await
            .map_err(|err| transport_error(err, timeout, url))
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

fn required_id<'a>(value: &'a str, what: &str) -> ApiResult<&'a str> {
    let value = value.trim();
    if value.is_empty() {
        return Err(ApiError::invalid_input(format!("缺少{what} id")));
    }
    Ok(value)
}

fn required_user_id(user_id: &str) -> ApiResult<&str> {
    required_id(user_id, "用户")
}

fn required_item_id(item_id: &str) -> ApiResult<&str> {
    required_id(item_id, "条目")
}

fn required_person_id(person_id: &str) -> ApiResult<&str> {
    required_id(person_id, "演员")
}

fn no_source_error() -> ApiError {
    ApiError::no_playable_source("该媒体没有可直接播放的源（需要转码，当前版本尚未实现）")
}

/// Chapters for the player: the media source's own list wins, unless it no
/// longer carries an intro marker and the item still does — Emby's playback
/// probe replaces an item's chapters with the probe result on a strm source's
/// first playback, so the source list can be the one that lost the markers.
fn playback_chapters(
    source: Option<Vec<RawChapter>>,
    item_chapters: Vec<EmbyChapter>,
) -> Vec<EmbyChapter> {
    let chapters = chapters_from_raw(source);
    if chapters.iter().any(has_intro_start) || !item_chapters.iter().any(has_intro_start) {
        return chapters;
    }
    item_chapters
}

fn has_intro_start(chapter: &EmbyChapter) -> bool {
    chapter.marker_type.as_deref() == Some("IntroStart")
}

/// Emby's `SortOrder` query value. Anything other than an explicit descending
/// value keeps the historical default (`Ascending`), so older callers that do
/// not send `sortOrder` are unaffected.
fn sort_order(value: Option<&str>) -> &'static str {
    match value.map(str::trim).filter(|value| !value.is_empty()) {
        Some(value)
            if value.eq_ignore_ascii_case("Descending") || value.eq_ignore_ascii_case("Desc") =>
        {
            "Descending"
        }
        _ => "Ascending",
    }
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
        ApiError::network(format!(
            "连接 Emby 服务器超时（{} 秒）：{target}",
            timeout.as_secs()
        ))
    } else if err.is_connect() {
        ApiError::network(format!(
            "无法连接 Emby 服务器，请检查地址与网络：{target}"
        ))
    } else if err.is_decode() {
        ApiError::decode(format!("Emby 响应无法解析：{target}"))
    } else {
        ApiError::network(format!("Emby 网络请求失败：{target}"))
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
        let (server_url, handled) = spawn_mock_sequence(vec![body]);
        let handle = std::thread::spawn(move || {
            handled
                .join()
                .expect("mock server thread")
                .pop()
                .expect("one request")
        });
        (server_url, handle)
    }

    /// Serves `bodies` in order, one fresh connection per request, and returns
    /// the raw request text of each (used by flows that issue more than one
    /// call, e.g. the item read before `PlaybackInfo`).
    fn spawn_mock_sequence(
        bodies: Vec<&'static str>,
    ) -> (String, std::thread::JoinHandle<Vec<String>>) {
        use std::net::TcpListener;

        let listener = TcpListener::bind("127.0.0.1:0").expect("bind mock server");
        let addr = listener.local_addr().expect("mock server address");
        let handle = std::thread::spawn(move || {
            bodies
                .into_iter()
                .map(|body| {
                    let (mut stream, _) = listener.accept().expect("accept request");
                    answer_mock_request(&mut stream, body)
                })
                .collect()
        });
        (format!("http://{addr}"), handle)
    }

    /// Reads one request off `stream` and answers it with `body`.
    fn answer_mock_request(stream: &mut std::net::TcpStream, body: &'static str) -> String {
        use std::io::{Read, Write};

        stream
            .set_read_timeout(Some(Duration::from_secs(5)))
            .expect("read timeout");

        let mut request = Vec::new();
        let mut chunk = [0_u8; 1024];
        let header_end = loop {
            let read = stream.read(&mut chunk).expect("read request");
            if read == 0 {
                break request.len();
            }
            request.extend_from_slice(&chunk[..read]);
            if let Some(start) = request.windows(4).position(|window| window == b"\r\n\r\n") {
                break start + 4;
            }
        };
        // POST bodies may arrive in a separate packet; drain the declared
        // Content-Length so tests can inspect JSON payloads too.
        let headers = String::from_utf8_lossy(&request[..header_end.min(request.len())]);
        let content_length = headers
            .lines()
            .find_map(|line| {
                line.split_once(':').and_then(|(name, value)| {
                    name.eq_ignore_ascii_case("content-length")
                        .then(|| value.trim().parse::<usize>().ok())
                        .flatten()
                })
            })
            .unwrap_or(0);
        let wanted = header_end + content_length;
        while request.len() < wanted {
            let read = stream.read(&mut chunk).expect("read request body");
            if read == 0 {
                break;
            }
            request.extend_from_slice(&chunk[..read]);
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
            request_line.contains("EnableImageTypes=Primary%2CThumb%2CBackdrop")
                || request_line.contains("EnableImageTypes=Primary,Thumb,Backdrop"),
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
    async fn playback_target_carries_source_chapters_and_marker_types() {
        const ITEM: &str = r#"{"Id": "i1", "Name": "Episode", "Type": "Episode"}"#;
        const INFO: &str = r#"{
            "PlaySessionId": "ps-top",
            "MediaSources": [{
                "Id": "src1",
                "DirectStreamUrl": "/videos/i1/stream?static=true&mediaSourceId=src1",
                "Chapters": [
                    {"StartPositionTicks": 0, "Name": "Intro", "MarkerType": "IntroStart"},
                    {"StartPositionTicks": 900000000, "Name": "Main", "MarkerType": "IntroEnd"}
                ]
            }]
        }"#;
        let (server_url, server) = spawn_mock_sequence(vec![ITEM, INFO]);
        let target = test_client(server_url)
            .playback_target(
                "u1",
                &EmbyPlaybackRequest {
                    item_id: "i1".to_string(),
                    start_seconds: Some(0.0),
                },
            )
            .await
            .expect("playback target");
        let requests = server.join().expect("mock server thread");
        assert_eq!(requests.len(), 2, "{requests:?}");
        assert!(
            requests[0].starts_with("GET /Users/u1/Items/i1?"),
            "{}",
            requests[0]
        );
        let request_line = requests[1].lines().next().unwrap_or_default();
        assert!(
            request_line.starts_with("POST /Items/i1/PlaybackInfo?UserId=u1 "),
            "{request_line}"
        );
        assert_eq!(target.chapters.len(), 2);
        assert_eq!(target.chapters[0].marker_type.as_deref(), Some("IntroStart"));
        assert_eq!(target.chapters[1].start_seconds, 90.0);
        assert_eq!(target.chapters[1].marker_type.as_deref(), Some("IntroEnd"));
    }

    /// Emby probes a strm source on its first playback and saves the probe
    /// result over the item's chapters, so `PlaybackInfo` can come back without
    /// the markers the item read (sent before the probe) still has.
    #[tokio::test]
    async fn playback_target_falls_back_to_item_chapters_when_the_source_lost_them() {
        const ITEM: &str = r#"{
            "Id": "i1",
            "Name": "Episode",
            "Type": "Episode",
            "UserData": {"PlaybackPositionTicks": 20000000},
            "Chapters": [
                {"StartPositionTicks": 0, "Name": "Intro", "MarkerType": "IntroStart"},
                {"StartPositionTicks": 670000000, "Name": "Main", "MarkerType": "IntroEnd"},
                {"StartPositionTicks": 1861600000, "Name": "Part 2"}
            ]
        }"#;
        const INFO: &str = r#"{
            "MediaSources": [{
                "Id": "src1",
                "DirectStreamUrl": "/videos/i1/stream?static=true&mediaSourceId=src1",
                "Chapters": [{"StartPositionTicks": 1861600000, "Name": "Part 2"}]
            }]
        }"#;
        let (server_url, server) = spawn_mock_sequence(vec![ITEM, INFO]);
        let target = test_client(server_url)
            .playback_target(
                "u1",
                &EmbyPlaybackRequest {
                    item_id: "i1".to_string(),
                    start_seconds: None,
                },
            )
            .await
            .expect("playback target");
        server.join().expect("mock server thread");
        // Resume point still comes from the item read.
        assert_eq!(target.start_seconds, 2.0);
        // Chapters come from the item, markers included.
        assert_eq!(target.chapters.len(), 3, "{:?}", target.chapters);
        assert_eq!(target.chapters[0].marker_type.as_deref(), Some("IntroStart"));
        assert_eq!(target.chapters[1].start_seconds, 67.0);
        assert_eq!(target.chapters[1].marker_type.as_deref(), Some("IntroEnd"));
    }

    /// When neither list has an intro marker, the source list stays as-is.
    #[tokio::test]
    async fn playback_target_keeps_the_source_chapters_without_markers() {
        const ITEM: &str = r#"{"Id": "i1", "Name": "Episode", "Type": "Episode"}"#;
        const INFO: &str = r#"{
            "MediaSources": [{
                "Id": "src1",
                "DirectStreamUrl": "/videos/i1/stream?static=true&mediaSourceId=src1",
                "Chapters": [{"StartPositionTicks": 4009200000, "Name": "Part 2"}]
            }]
        }"#;
        let (server_url, server) = spawn_mock_sequence(vec![ITEM, INFO]);
        let target = test_client(server_url)
            .playback_target(
                "u1",
                &EmbyPlaybackRequest {
                    item_id: "i1".to_string(),
                    start_seconds: Some(0.0),
                },
            )
            .await
            .expect("playback target");
        server.join().expect("mock server thread");
        assert_eq!(target.chapters.len(), 1);
        assert_eq!(target.chapters[0].start_seconds, 400.92);
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
        for field in [
            "DateCreated",
            "People",
            "Studios",
            "Taglines",
            "MediaStreams",
            "MediaSources",
        ] {
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

    #[tokio::test]
    async fn items_sends_sort_order_filters_and_genres() {
        const PAGE: &str = r#"{"Items":[],"TotalRecordCount":0,"StartIndex":0}"#;
        let (server_url, server) = spawn_mock_server(PAGE);
        let req = EmbyItemsRequest {
            parent_id: Some("v1".to_string()),
            sort_by: Some("DateCreated".to_string()),
            sort_order: Some("Descending".to_string()),
            filters: Some("IsUnplayed".to_string()),
            genres: Some("Animation,Short".to_string()),
            limit: Some(24),
            ..EmbyItemsRequest::default()
        };

        let page = test_client(server_url)
            .items("u1", &req)
            .await
            .expect("items request");
        let request = server.join().expect("mock server thread");
        let request_line = request.lines().next().unwrap_or_default();

        assert!(
            request_line.starts_with("GET /Users/u1/Items?"),
            "{request_line}"
        );
        assert!(request_line.contains("SortBy=DateCreated"), "{request_line}");
        assert!(request_line.contains("SortOrder=Descending"), "{request_line}");
        assert!(request_line.contains("Filters=IsUnplayed"), "{request_line}");
        assert!(
            request_line.contains("Genres=Animation%2CShort")
                || request_line.contains("Genres=Animation,Short"),
            "{request_line}"
        );
        assert!(request_line.contains("Limit=24"), "{request_line}");
        assert!(
            request_line.contains("EnableImageTypes=Primary%2CThumb%2CBackdrop")
                || request_line.contains("EnableImageTypes=Primary,Thumb,Backdrop"),
            "{request_line}"
        );
        assert!(page.items.is_empty());
    }

    #[tokio::test]
    async fn items_defaults_to_ascending_and_normalizes_case() {
        const PAGE: &str = r#"{"Items":[],"TotalRecordCount":0,"StartIndex":0}"#;

        let (server_url, server) = spawn_mock_server(PAGE);
        test_client(server_url)
            .items("u1", &EmbyItemsRequest::default())
            .await
            .expect("items request");
        let request = server.join().expect("mock server thread");
        let request_line = request.lines().next().unwrap_or_default();
        assert!(request_line.contains("SortOrder=Ascending"), "{request_line}");
        assert!(!request_line.contains("Genres="), "{request_line}");

        let (server_url, server) = spawn_mock_server(PAGE);
        let req = EmbyItemsRequest {
            sort_order: Some(" descending ".to_string()),
            ..EmbyItemsRequest::default()
        };
        test_client(server_url)
            .items("u1", &req)
            .await
            .expect("items request");
        let request = server.join().expect("mock server thread");
        let request_line = request.lines().next().unwrap_or_default();
        assert!(request_line.contains("SortOrder=Descending"), "{request_line}");
    }

    #[tokio::test]
    async fn mark_played_uses_post_and_delete() {
        const EMPTY: &str = "{}";

        let (server_url, server) = spawn_mock_server(EMPTY);
        test_client(server_url)
            .set_played("u1", "i1", true)
            .await
            .expect("mark played");
        let request = server.join().expect("mock server thread");
        assert!(
            request
                .lines()
                .next()
                .unwrap_or_default()
                .starts_with("POST /Users/u1/PlayedItems/i1 "),
            "{request}"
        );

        let (server_url, server) = spawn_mock_server(EMPTY);
        test_client(server_url)
            .set_played("u1", "i1", false)
            .await
            .expect("mark unplayed");
        let request = server.join().expect("mock server thread");
        assert!(
            request
                .lines()
                .next()
                .unwrap_or_default()
                .starts_with("DELETE /Users/u1/PlayedItems/i1 "),
            "{request}"
        );
    }

    #[tokio::test]
    async fn favorite_uses_post_and_delete() {
        const EMPTY: &str = "{}";

        let (server_url, server) = spawn_mock_server(EMPTY);
        test_client(server_url)
            .set_favorite("u1", "i1", true)
            .await
            .expect("add favorite");
        let request = server.join().expect("mock server thread");
        assert!(
            request
                .lines()
                .next()
                .unwrap_or_default()
                .starts_with("POST /Users/u1/FavoriteItems/i1 "),
            "{request}"
        );

        let (server_url, server) = spawn_mock_server(EMPTY);
        test_client(server_url)
            .set_favorite("u1", "i1", false)
            .await
            .expect("remove favorite");
        let request = server.join().expect("mock server thread");
        assert!(
            request
                .lines()
                .next()
                .unwrap_or_default()
                .starts_with("DELETE /Users/u1/FavoriteItems/i1 "),
            "{request}"
        );
    }

    #[tokio::test]
    async fn person_items_uses_the_persons_endpoint_and_reuses_the_item_parser() {
        const PAGE: &str = r#"{
            "Items": [
                {"Id": "m1", "Name": "First", "Type": "Movie", "Genres": ["Drama"],
                 "UserData": {"Played": true, "PlaybackPositionTicks": 0}},
                {"Id": "s1", "Name": "Show", "Type": "Series"}
            ],
            "TotalRecordCount": 2,
            "StartIndex": 0
        }"#;
        let (server_url, server) = spawn_mock_server(PAGE);

        let page = test_client(server_url)
            .person_items("u1", "p1", 60)
            .await
            .expect("person items request");
        let request = server.join().expect("mock server thread");
        let request_line = request.lines().next().unwrap_or_default();

        assert!(
            request_line.starts_with("GET /Persons/p1/Items?"),
            "{request_line}"
        );
        assert!(request_line.contains("UserId=u1"), "{request_line}");
        assert!(request_line.contains("Recursive=true"), "{request_line}");
        assert!(request_line.contains("EnableUserData=true"), "{request_line}");
        assert!(
            request_line.contains("Fields=PrimaryImageAspectRatio"),
            "{request_line}"
        );
        assert!(request_line.contains("Limit=60"), "{request_line}");
        assert_eq!(page.total_record_count, 2);
        assert_eq!(page.items.len(), 2);
        assert_eq!(page.items[0].id, "m1");
        assert_eq!(page.items[0].genres, vec!["Drama"]);
        assert!(page.items[0].user_data.played);
        assert_eq!(page.items[1].r#type, "Series");
    }

    #[tokio::test]
    async fn person_items_with_zero_limit_omits_limit_and_rejects_empty_id() {
        const PAGE: &str = r#"{"Items":[],"TotalRecordCount":0,"StartIndex":0}"#;
        let (server_url, server) = spawn_mock_server(PAGE);

        test_client(server_url)
            .person_items("u1", "p1", 0)
            .await
            .expect("person items request");
        let request = server.join().expect("mock server thread");
        let request_line = request.lines().next().unwrap_or_default();
        // Note: `ImageTypeLimit=1` contains the substring "Limit=", so match
        // whole query parameters instead.
        let query = request_line.split('?').nth(1).unwrap_or_default();
        assert!(
            !query.split('&').any(|pair| pair.starts_with("Limit=")),
            "{request_line}"
        );

        let client = test_client("http://127.0.0.1:1".to_string());
        let error = client
            .person_items("u1", "  ", 10)
            .await
            .expect_err("empty person id");
        assert_eq!(error.kind(), ApiErrorKind::InvalidInput);
    }

    #[tokio::test]
    async fn delete_items_posts_ids_and_delete_info_returns_paths() {
        let (server_url, server) = spawn_mock_server("{}");
        test_client(server_url)
            .delete_items(&["m1".to_string(), " m2 ".to_string()])
            .await
            .expect("delete items");
        let request = server.join().expect("mock server thread");
        let request_line = request.lines().next().unwrap_or_default();
        assert!(
            request_line.starts_with("POST /Items/Delete?"),
            "{request_line}"
        );
        assert!(request_line.contains("Ids=m1"), "{request_line}");
        assert!(request_line.contains("m2"), "{request_line}");

        let (server_url, server) =
            spawn_mock_server(r#"{"Paths":["/media/m1.mkv","/media/m2.mkv"]}"#);
        let paths = test_client(server_url)
            .delete_info("m1")
            .await
            .expect("delete info");
        assert_eq!(paths, vec!["/media/m1.mkv", "/media/m2.mkv"]);
        let request = server.join().expect("mock server thread");
        assert!(
            request
                .lines()
                .next()
                .unwrap_or_default()
                .starts_with("GET /Items/m1/DeleteInfo "),
            "{request}"
        );
    }

    #[test]
    fn remote_search_image_url_uses_the_proxy_endpoint() {
        let client = EmbyClient::with_token(
            default_http_client().expect("http client"),
            "http://emby.test:8096".to_string(),
            "test-device",
            "Test",
            "test-token",
        );
        let url = client
            .remote_search_image_url("https://image.test/poster.jpg", Some("TheMovieDb"))
            .expect("image url");
        assert!(
            url.starts_with("http://emby.test:8096/Items/RemoteSearch/Image?"),
            "{url}"
        );
        assert!(url.contains("ProviderName=TheMovieDb"), "{url}");
        assert!(url.contains("api_key=test-token"), "{url}");
        assert!(
            url.contains("imageUrl=https%3A%2F%2Fimage.test%2Fposter.jpg"),
            "{url}"
        );
    }

    #[tokio::test]
    async fn remote_search_sends_official_payload_and_apply_round_trips_it() {
        let response =
            r#"[{"Name":"Matched","ProductionYear":2001,"SearchProviderName":"Provider"}]"#;
        let (server_url, server) = spawn_mock_server(response);
        let results = test_client(server_url)
            .remote_search(&EmbyRemoteSearchRequest {
                item_id: "i1".to_string(),
                item_type: "Movie".to_string(),
                name: "Lookup".to_string(),
                production_year: Some(2001),
            })
            .await
            .expect("remote search");
        assert_eq!(results.len(), 1);
        assert_eq!(results[0]["Name"], "Matched");
        let request = server.join().expect("mock server thread");
        let request_line = request.lines().next().unwrap_or_default();
        assert!(
            request_line.starts_with("POST /Items/RemoteSearch/Movie "),
            "{request_line}"
        );
        assert!(request.contains(r#""ItemId":"i1""#), "{request}");
        assert!(request.contains(r#""Name":"Lookup""#), "{request}");
        assert!(request.contains(r#""Year":2001"#), "{request}");

        let (server_url, server) = spawn_mock_server("{}");
        let result = serde_json::json!({"Name":"Matched","ProviderIds":{"Tmdb":"1"}});
        test_client(server_url)
            .apply_remote_search(&EmbyApplyRemoteSearchRequest {
                item_id: "i1".to_string(),
                result: result.clone(),
                replace_all_images: true,
            })
            .await
            .expect("apply search");
        let request = server.join().expect("mock server thread");
        let request_line = request.lines().next().unwrap_or_default();
        assert!(
            request_line.starts_with("POST /Items/RemoteSearch/Apply/i1?"),
            "{request_line}"
        );
        assert!(
            request_line.contains("ReplaceAllImages=true"),
            "{request_line}"
        );
        assert!(request.contains(r#""ProviderIds""#), "{request}");
    }

    #[test]
    fn item_fields_cover_the_frontend_contract() {
        for field in [
            "PrimaryImageAspectRatio",
            "Overview",
            "Genres",
            "UserData",
            "DateCreated",
        ] {
            assert!(ITEM_FIELDS.contains(field), "missing {field}");
        }
        assert!(
            DETAIL_FIELDS.contains("DateCreated"),
            "detail fields must include DateCreated"
        );
    }
}
