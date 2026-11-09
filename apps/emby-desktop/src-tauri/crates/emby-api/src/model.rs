//! Emby wire types.
//!
//! Two layers:
//! - `Raw*` structs deserialize the server's PascalCase JSON
//!   (`Id`, `Name`, `MediaSources`, ...). Field names here must stay aligned
//!   with Emby; renaming them silently drops data.
//! - The public `Emby*` structs are the TypeScript contract. They serialize as
//!   camelCase and never contain the access token. The frontend consumes these
//!   through the Tauri commands in `src/emby.rs`.

use std::collections::HashMap;

use serde::{Deserialize, Serialize};

/// Default image width used by `emby_image_url`.
pub const DEFAULT_IMAGE_WIDTH: u32 = 400;

/// Session information exposed to the frontend. Deliberately has no token.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct EmbySession {
    pub server_url: String,
    pub user_id: String,
    pub user_name: String,
    pub server_name: String,
    pub device_id: String,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct EmbyView {
    pub id: String,
    pub name: String,
    pub collection_type: Option<String>,
}

#[derive(Debug, Clone, Default, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct EmbyUserData {
    pub playback_position_ticks: Option<i64>,
    pub played: bool,
    pub unplayed_item_count: Option<i32>,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct EmbyItem {
    pub id: String,
    pub name: String,
    #[serde(rename = "type")]
    pub r#type: String,
    pub media_type: Option<String>,
    pub production_year: Option<i32>,
    pub run_time_ticks: Option<i64>,
    pub image_tags: HashMap<String, String>,
    pub backdrop_image_tags: Vec<String>,
    pub primary_image_aspect_ratio: Option<f64>,
    pub user_data: EmbyUserData,
    pub series_id: Option<String>,
    pub series_name: Option<String>,
    pub index_number: Option<i32>,
    pub parent_index_number: Option<i32>,
    pub overview: Option<String>,
    pub genres: Vec<String>,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct EmbyItemsPage {
    pub items: Vec<EmbyItem>,
    pub total_record_count: u32,
    pub start_index: u32,
}

/// Resolved playback target handed to the frontend/`player_open`.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct EmbyPlaybackTarget {
    pub item_id: String,
    pub url: String,
    pub play_session_id: Option<String>,
    pub media_source_id: Option<String>,
    pub start_seconds: f64,
}

#[derive(Debug, Clone, Default, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", default)]
pub struct EmbyLoginRequest {
    pub server: String,
    pub username: String,
    pub password: String,
}

#[derive(Debug, Clone, Default, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", default)]
pub struct EmbyItemsRequest {
    pub parent_id: Option<String>,
    pub start_index: Option<u32>,
    pub limit: Option<u32>,
    pub search_term: Option<String>,
    pub include_item_types: Option<String>,
    pub recursive: Option<bool>,
    pub sort_by: Option<String>,
    pub filters: Option<String>,
}

#[derive(Debug, Clone, Default, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", default)]
pub struct EmbyResumeRequest {
    pub limit: Option<u32>,
}

#[derive(Debug, Clone, Default, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", default)]
pub struct EmbyPlaybackRequest {
    pub item_id: String,
    pub start_seconds: Option<f64>,
}

#[derive(Debug, Clone, Default, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", default)]
pub struct EmbyReportRequest {
    pub item_id: String,
    pub media_source_id: Option<String>,
    pub play_session_id: Option<String>,
    pub position_seconds: f64,
    pub paused: bool,
    /// `start` | `progress` | `pause` | `unpause` | `seek` | `stop` | `ended`
    pub event: String,
}

#[derive(Debug, Clone, Default, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", default)]
pub struct EmbyImageRequest {
    pub item_id: String,
    pub image_type: Option<String>,
    pub max_width: Option<u32>,
    pub tag: Option<String>,
}

// ---------------------------------------------------------------------------
// Raw Emby JSON (PascalCase). Keep field names aligned with the API.
// ---------------------------------------------------------------------------

#[derive(Debug, Default, Deserialize)]
#[serde(rename_all = "PascalCase", default)]
pub(crate) struct RawItem {
    pub id: Option<String>,
    pub name: Option<String>,
    #[serde(rename = "Type")]
    pub item_type: Option<String>,
    pub media_type: Option<String>,
    pub production_year: Option<i32>,
    pub run_time_ticks: Option<i64>,
    pub image_tags: Option<HashMap<String, String>>,
    pub backdrop_image_tags: Option<Vec<String>>,
    pub primary_image_aspect_ratio: Option<f64>,
    pub user_data: Option<RawUserData>,
    pub series_id: Option<String>,
    pub series_name: Option<String>,
    pub index_number: Option<i32>,
    pub parent_index_number: Option<i32>,
    pub overview: Option<String>,
    pub genres: Option<Vec<String>>,
    pub collection_type: Option<String>,
}

#[derive(Debug, Default, Deserialize)]
#[serde(rename_all = "PascalCase", default)]
pub(crate) struct RawUserData {
    pub playback_position_ticks: Option<i64>,
    pub played: Option<bool>,
    pub unplayed_item_count: Option<i32>,
}

#[derive(Debug, Default, Deserialize)]
#[serde(rename_all = "PascalCase", default)]
pub(crate) struct RawItemsResponse {
    pub items: Option<Vec<RawItem>>,
    pub total_record_count: Option<u32>,
    pub start_index: Option<u32>,
}

#[derive(Debug, Default, Deserialize)]
#[serde(rename_all = "PascalCase", default)]
pub(crate) struct RawAuthResponse {
    pub access_token: Option<String>,
    pub server_id: Option<String>,
    pub server_name: Option<String>,
    pub user: Option<RawAuthUser>,
}

#[derive(Debug, Default, Deserialize)]
#[serde(rename_all = "PascalCase", default)]
pub(crate) struct RawAuthUser {
    pub id: Option<String>,
    pub name: Option<String>,
}

#[derive(Debug, Default, Deserialize)]
#[serde(rename_all = "PascalCase", default)]
pub(crate) struct RawPublicSystemInfo {
    pub server_name: Option<String>,
}

#[derive(Debug, Default, Deserialize)]
#[serde(rename_all = "PascalCase", default)]
pub(crate) struct RawPlaybackInfo {
    pub play_session_id: Option<String>,
    pub media_sources: Option<Vec<RawMediaSource>>,
}

#[derive(Debug, Default, Deserialize)]
#[serde(rename_all = "PascalCase", default)]
pub(crate) struct RawMediaSource {
    pub id: Option<String>,
    pub direct_stream_url: Option<String>,
    pub play_session_id: Option<String>,
}

impl From<RawUserData> for EmbyUserData {
    fn from(raw: RawUserData) -> Self {
        Self {
            playback_position_ticks: raw.playback_position_ticks,
            played: raw.played.unwrap_or(false),
            unplayed_item_count: raw.unplayed_item_count,
        }
    }
}

impl RawItem {
    pub(crate) fn into_item(self) -> EmbyItem {
        EmbyItem {
            id: self.id.unwrap_or_default(),
            name: self.name.unwrap_or_default(),
            r#type: self.item_type.unwrap_or_default(),
            media_type: self.media_type,
            production_year: self.production_year,
            run_time_ticks: self.run_time_ticks,
            image_tags: self.image_tags.unwrap_or_default(),
            backdrop_image_tags: self.backdrop_image_tags.unwrap_or_default(),
            primary_image_aspect_ratio: self.primary_image_aspect_ratio,
            user_data: self.user_data.unwrap_or_default().into(),
            series_id: self.series_id,
            series_name: self.series_name,
            index_number: self.index_number,
            parent_index_number: self.parent_index_number,
            overview: trim_optional(self.overview),
            genres: self.genres.unwrap_or_default(),
        }
    }

    pub(crate) fn into_view(self) -> EmbyView {
        EmbyView {
            id: self.id.unwrap_or_default(),
            name: self.name.unwrap_or_default(),
            collection_type: trim_optional(self.collection_type),
        }
    }
}

impl RawItemsResponse {
    pub(crate) fn into_page(self) -> EmbyItemsPage {
        EmbyItemsPage {
            items: self
                .items
                .unwrap_or_default()
                .into_iter()
                .map(RawItem::into_item)
                .collect(),
            total_record_count: self.total_record_count.unwrap_or(0),
            start_index: self.start_index.unwrap_or(0),
        }
    }
}

/// Picks the media source to play:
/// 1. the first source that already has a `DirectStreamUrl` (server-built URL),
/// 2. otherwise the first source that has an `Id` (fallback stream URL).
///
/// `None` means the item needs transcoding, which this client does not do.
pub(crate) fn pick_playback_source(info: &RawPlaybackInfo) -> Option<&RawMediaSource> {
    let sources = info.media_sources.as_deref().unwrap_or_default();
    sources
        .iter()
        .find(|source| non_empty(source.direct_stream_url.as_deref()))
        .or_else(|| {
            sources
                .iter()
                .find(|source| non_empty(source.id.as_deref()))
        })
}

pub(crate) fn non_empty(value: Option<&str>) -> bool {
    value.map(|value| !value.trim().is_empty()).unwrap_or(false)
}

fn trim_optional(value: Option<String>) -> Option<String> {
    value
        .map(|value| value.trim().to_string())
        .filter(|value| !value.is_empty())
}

#[cfg(test)]
mod tests {
    use super::*;

    const ITEMS_PAGE: &str = r#"{
        "Items": [
            {
                "Id": "item1",
                "Name": "Big Buck Bunny",
                "Type": "Movie",
                "MediaType": "Video",
                "ProductionYear": 2008,
                "RunTimeTicks": 5960000000,
                "Overview": "A rabbit.",
                "Genres": ["Animation", "Short"],
                "ImageTags": {"Primary": "tag-p"},
                "BackdropImageTags": ["tag-b"],
                "PrimaryImageAspectRatio": 1.7777778,
                "UserData": {"PlaybackPositionTicks": 50000000, "Played": false, "UnplayedItemCount": 2}
            },
            {
                "Id": "episode2",
                "Name": "Episode 2",
                "Type": "Episode",
                "MediaType": "Video",
                "SeriesId": "series1",
                "SeriesName": "The Show",
                "IndexNumber": 2,
                "ParentIndexNumber": 1,
                "ImageTags": null,
                "UserData": null
            }
        ],
        "TotalRecordCount": 42,
        "StartIndex": 10
    }"#;

    #[test]
    fn parses_items_page_and_keeps_pascal_case_fields() {
        let raw: RawItemsResponse = serde_json::from_str(ITEMS_PAGE).unwrap();
        let page = raw.into_page();
        assert_eq!(page.total_record_count, 42);
        assert_eq!(page.start_index, 10);
        assert_eq!(page.items.len(), 2);

        let movie = &page.items[0];
        assert_eq!(movie.id, "item1");
        assert_eq!(movie.name, "Big Buck Bunny");
        assert_eq!(movie.r#type, "Movie");
        assert_eq!(movie.media_type.as_deref(), Some("Video"));
        assert_eq!(movie.production_year, Some(2008));
        assert_eq!(movie.run_time_ticks, Some(5_960_000_000));
        assert_eq!(movie.overview.as_deref(), Some("A rabbit."));
        assert_eq!(movie.genres, vec!["Animation", "Short"]);
        assert_eq!(movie.image_tags.get("Primary").map(String::as_str), Some("tag-p"));
        assert_eq!(movie.backdrop_image_tags, vec!["tag-b"]);
        assert_eq!(movie.primary_image_aspect_ratio, Some(1.7777778));
        assert_eq!(
            movie.user_data.playback_position_ticks,
            Some(50_000_000)
        );
        assert!(!movie.user_data.played);
        assert_eq!(movie.user_data.unplayed_item_count, Some(2));

        let episode = &page.items[1];
        assert_eq!(episode.series_id.as_deref(), Some("series1"));
        assert_eq!(episode.series_name.as_deref(), Some("The Show"));
        assert_eq!(episode.index_number, Some(2));
        assert_eq!(episode.parent_index_number, Some(1));
        assert!(episode.image_tags.is_empty(), "null ImageTags tolerated");
        assert_eq!(episode.user_data, EmbyUserData::default());
        assert_eq!(episode.user_data.playback_position_ticks, None);
        assert!(!episode.user_data.played);
    }

    #[test]
    fn public_types_serialize_as_camel_case_for_the_frontend() {
        let raw: RawItemsResponse = serde_json::from_str(ITEMS_PAGE).unwrap();
        let page = raw.into_page();
        let value = serde_json::to_value(&page).unwrap();

        assert!(value.get("totalRecordCount").is_some());
        assert!(value.get("startIndex").is_some());
        assert!(value.get("TotalRecordCount").is_none());

        let item = &value["items"][0];
        for key in [
            "id",
            "name",
            "type",
            "mediaType",
            "productionYear",
            "runTimeTicks",
            "imageTags",
            "backdropImageTags",
            "primaryImageAspectRatio",
            "userData",
            "seriesId",
            "seriesName",
            "indexNumber",
            "parentIndexNumber",
            "overview",
            "genres",
        ] {
            assert!(item.get(key).is_some(), "missing frontend field {key}");
        }
        assert!(item.get("image_tags").is_none());
        let user_data = &item["userData"];
        assert!(user_data.get("playbackPositionTicks").is_some());
        assert!(user_data.get("played").is_some());
        assert!(user_data.get("unplayedItemCount").is_some());
    }

    #[test]
    fn session_view_and_target_serialize_camel_case() {
        let session = EmbySession {
            server_url: "http://host:8096".into(),
            user_id: "u1".into(),
            user_name: "alice".into(),
            server_name: "Home".into(),
            device_id: "dev".into(),
        };
        let value = serde_json::to_value(&session).unwrap();
        assert_eq!(value["serverUrl"], "http://host:8096");
        assert!(value.get("accessToken").is_none());
        assert!(value.get("token").is_none());

        let view = EmbyView {
            id: "v1".into(),
            name: "Movies".into(),
            collection_type: Some("movies".into()),
        };
        assert_eq!(serde_json::to_value(&view).unwrap()["collectionType"], "movies");

        let target = EmbyPlaybackTarget {
            item_id: "i1".into(),
            url: "http://host/videos/i1/stream?api_key=k".into(),
            play_session_id: Some("ps".into()),
            media_source_id: Some("s1".into()),
            start_seconds: 12.5,
        };
        let value = serde_json::to_value(&target).unwrap();
        assert_eq!(value["itemId"], "i1");
        assert_eq!(value["playSessionId"], "ps");
        assert_eq!(value["mediaSourceId"], "s1");
        assert_eq!(value["startSeconds"], 12.5);
    }

    #[test]
    fn parses_views_and_playback_info() {
        let views: RawItemsResponse = serde_json::from_str(
            r#"{"Items":[{"Id":"v1","Name":"Movies","CollectionType":"movies","Type":"CollectionFolder"}]}"#,
        )
        .unwrap();
        let view = views.items.unwrap().remove(0).into_view();
        assert_eq!(view.id, "v1");
        assert_eq!(view.name, "Movies");
        assert_eq!(view.collection_type.as_deref(), Some("movies"));

        let info: RawPlaybackInfo = serde_json::from_str(
            r#"{
                "PlaySessionId": "top",
                "MediaSources": [
                    {"Id": "s1", "Container": "mkv", "DirectStreamUrl": "/videos/i1/stream?static=true&mediaSourceId=s1"},
                    {"Id": "s2", "Container": "mp4"}
                ]
            }"#,
        )
        .unwrap();
        let source = pick_playback_source(&info).unwrap();
        assert_eq!(source.id.as_deref(), Some("s1"));
        assert!(source.direct_stream_url.is_some());
        assert_eq!(info.play_session_id.as_deref(), Some("top"));
    }

    #[test]
    fn playback_source_falls_back_to_first_id() {
        let info: RawPlaybackInfo = serde_json::from_str(
            r#"{"MediaSources":[
                {"Container":"mp4"},
                {"Id":"s2","Container":"mkv","DirectStreamUrl":"   "},
                {"Id":"s3","Container":"avi"}
            ]}"#,
        )
        .unwrap();
        let source = pick_playback_source(&info).unwrap();
        assert_eq!(source.id.as_deref(), Some("s2"));
        assert!(source.direct_stream_url.as_deref().unwrap().trim().is_empty());
    }

    #[test]
    fn playback_source_is_none_when_sources_are_empty() {
        let info: RawPlaybackInfo = serde_json::from_str(r#"{"MediaSources":[]}"#).unwrap();
        assert!(pick_playback_source(&info).is_none());
        let missing: RawPlaybackInfo = serde_json::from_str("{}").unwrap();
        assert!(pick_playback_source(&missing).is_none());
    }

    #[test]
    fn request_types_accept_camel_case_and_defaults() {
        let items: EmbyItemsRequest = serde_json::from_str(
            r#"{"parentId":"v1","startIndex":5,"limit":20,"searchTerm":"bunny","includeItemTypes":"Movie","recursive":false,"sortBy":"DateCreated","filters":"IsUnplayed"}"#,
        )
        .unwrap();
        assert_eq!(items.parent_id.as_deref(), Some("v1"));
        assert_eq!(items.start_index, Some(5));
        assert_eq!(items.limit, Some(20));
        assert_eq!(items.search_term.as_deref(), Some("bunny"));
        assert_eq!(items.include_item_types.as_deref(), Some("Movie"));
        assert_eq!(items.recursive, Some(false));
        assert_eq!(items.sort_by.as_deref(), Some("DateCreated"));
        assert_eq!(items.filters.as_deref(), Some("IsUnplayed"));

        let empty: EmbyItemsRequest = serde_json::from_str("{}").unwrap();
        assert_eq!(empty, EmbyItemsRequest::default());

        let report: EmbyReportRequest = serde_json::from_str(
            r#"{"itemId":"i1","positionSeconds":1.5,"paused":true,"event":"pause"}"#,
        )
        .unwrap();
        assert_eq!(report.item_id, "i1");
        assert_eq!(report.position_seconds, 1.5);
        assert!(report.paused);
        assert_eq!(report.event, "pause");

        let image: EmbyImageRequest =
            serde_json::from_str(r#"{"itemId":"i1"}"#).unwrap();
        assert_eq!(image.image_type, None);
        assert_eq!(image.max_width, None);

        let resume: EmbyResumeRequest = serde_json::from_str("{}").unwrap();
        assert_eq!(resume.limit, None);
        let resume: EmbyResumeRequest = serde_json::from_str(r#"{"limit":24}"#).unwrap();
        assert_eq!(resume.limit, Some(24));
    }
}
