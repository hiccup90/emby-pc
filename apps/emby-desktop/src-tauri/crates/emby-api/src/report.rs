//! Playback progress reporting: event -> endpoint/`EventName` mapping and the
//! JSON body sent to `/Sessions/Playing*`.

use serde::Serialize;

use crate::error::{ApiError, ApiResult};
use crate::model::EmbyReportRequest;
use crate::ticks::seconds_to_ticks;

/// Frontend playback event. The string values are part of the command contract
/// (`emby_report_playback`): `start|progress|pause|unpause|seek|stop|ended`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ReportEvent {
    Start,
    Progress,
    Pause,
    Unpause,
    Seek,
    Stop,
    Ended,
}

impl ReportEvent {
    pub fn parse(raw: &str) -> ApiResult<Self> {
        match raw.trim().to_ascii_lowercase().as_str() {
            "start" => Ok(Self::Start),
            "progress" => Ok(Self::Progress),
            "pause" => Ok(Self::Pause),
            "unpause" => Ok(Self::Unpause),
            "seek" => Ok(Self::Seek),
            "stop" => Ok(Self::Stop),
            "ended" => Ok(Self::Ended),
            other => Err(ApiError::unsupported(format!(
                "未知的播放上报事件：{other}（支持 start/progress/pause/unpause/seek/stop/ended）"
            ))),
        }
    }

    /// Path segments of the Emby endpoint for this event.
    pub fn segments(self) -> &'static [&'static str] {
        match self {
            Self::Start => &["Sessions", "Playing"],
            Self::Progress | Self::Pause | Self::Unpause | Self::Seek => {
                &["Sessions", "Playing", "Progress"]
            }
            Self::Stop | Self::Ended => &["Sessions", "Playing", "Stopped"],
        }
    }

    /// `EventName` in the body. `start` omits it, matching the Emby clients.
    pub fn event_name(self) -> Option<&'static str> {
        match self {
            Self::Start => None,
            Self::Progress | Self::Seek => Some("TimeUpdate"),
            Self::Pause => Some("Pause"),
            Self::Unpause => Some("Unpause"),
            Self::Stop | Self::Ended => Some("Stop"),
        }
    }

    /// Pause/Unpause are authoritative about the paused flag; other events use
    /// the flag reported by the player.
    pub(crate) fn is_paused(self, requested: bool) -> bool {
        match self {
            Self::Pause => true,
            Self::Unpause => false,
            _ => requested,
        }
    }
}

/// Body for `POST /Sessions/Playing`, `/Sessions/Playing/Progress` and
/// `/Sessions/Playing/Stopped`. Field names are the Emby (PascalCase) ones.
#[derive(Debug, Serialize)]
#[serde(rename_all = "PascalCase")]
pub(crate) struct ReportBody {
    item_id: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    media_source_id: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    play_session_id: Option<String>,
    position_ticks: i64,
    is_paused: bool,
    is_muted: bool,
    can_seek: bool,
    play_method: &'static str,
    #[serde(skip_serializing_if = "Option::is_none")]
    event_name: Option<&'static str>,
}

impl ReportBody {
    pub(crate) fn new(req: &EmbyReportRequest, event: ReportEvent) -> Self {
        Self {
            item_id: req.item_id.trim().to_string(),
            media_source_id: clean(req.media_source_id.as_deref()),
            play_session_id: clean(req.play_session_id.as_deref()),
            position_ticks: seconds_to_ticks(req.position_seconds),
            is_paused: event.is_paused(req.paused),
            is_muted: false,
            can_seek: true,
            // Direct play/stream only: the URL resolved by `playback_target` is
            // always a direct stream URL.
            play_method: "DirectStream",
            event_name: event.event_name(),
        }
    }
}

fn clean(value: Option<&str>) -> Option<String> {
    value
        .map(str::trim)
        .filter(|value| !value.is_empty())
        .map(str::to_string)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn request(event: &str, paused: bool) -> EmbyReportRequest {
        EmbyReportRequest {
            item_id: "item1".into(),
            media_source_id: Some("src1".into()),
            play_session_id: Some("ps1".into()),
            position_seconds: 12.5,
            paused,
            event: event.into(),
        }
    }

    #[test]
    fn maps_events_to_endpoints_and_event_names() {
        let cases = [
            ("start", &["Sessions", "Playing"][..], None),
            (
                "progress",
                &["Sessions", "Playing", "Progress"][..],
                Some("TimeUpdate"),
            ),
            ("pause", &["Sessions", "Playing", "Progress"][..], Some("Pause")),
            (
                "unpause",
                &["Sessions", "Playing", "Progress"][..],
                Some("Unpause"),
            ),
            (
                "seek",
                &["Sessions", "Playing", "Progress"][..],
                Some("TimeUpdate"),
            ),
            ("stop", &["Sessions", "Playing", "Stopped"][..], Some("Stop")),
            ("ended", &["Sessions", "Playing", "Stopped"][..], Some("Stop")),
        ];
        for (raw, segments, event_name) in cases {
            let event = ReportEvent::parse(raw).unwrap();
            assert_eq!(event.segments(), segments, "endpoint for {raw}");
            assert_eq!(event.event_name(), event_name, "EventName for {raw}");
        }
    }

    #[test]
    fn event_names_are_case_insensitive_and_trimmed() {
        assert_eq!(ReportEvent::parse(" PAUSE ").unwrap(), ReportEvent::Pause);
        assert_eq!(ReportEvent::parse("Ended").unwrap(), ReportEvent::Ended);
    }

    #[test]
    fn unknown_event_is_a_readable_error() {
        let err = ReportEvent::parse("buffering").unwrap_err();
        assert!(err.to_string().contains("buffering"), "{err}");
        assert!(ReportEvent::parse("").is_err());
    }

    #[test]
    fn body_uses_pascal_case_and_ticks() {
        let body = ReportBody::new(&request("progress", false), ReportEvent::Progress);
        let value = serde_json::to_value(&body).unwrap();
        assert_eq!(value["ItemId"], "item1");
        assert_eq!(value["MediaSourceId"], "src1");
        assert_eq!(value["PlaySessionId"], "ps1");
        assert_eq!(value["PositionTicks"], 125_000_000);
        assert_eq!(value["PositionTicks"].as_i64(), Some(125_000_000));
        assert_eq!(value["IsPaused"], false);
        assert_eq!(value["IsMuted"], false);
        assert_eq!(value["CanSeek"], true);
        assert_eq!(value["PlayMethod"], "DirectStream");
        assert_eq!(value["EventName"], "TimeUpdate");
        assert!(value.get("PositionSeconds").is_none());
    }

    #[test]
    fn start_body_omits_event_name() {
        let body = ReportBody::new(&request("start", false), ReportEvent::Start);
        let value = serde_json::to_value(&body).unwrap();
        assert!(value.get("EventName").is_none(), "{value}");
        assert_eq!(value["PositionTicks"], 125_000_000);
    }

    #[test]
    fn pause_and_unpause_override_the_paused_flag() {
        let body = ReportBody::new(&request("pause", false), ReportEvent::Pause);
        assert_eq!(serde_json::to_value(&body).unwrap()["IsPaused"], true);

        let body = ReportBody::new(&request("unpause", true), ReportEvent::Unpause);
        assert_eq!(serde_json::to_value(&body).unwrap()["IsPaused"], false);

        let body = ReportBody::new(&request("progress", true), ReportEvent::Progress);
        assert_eq!(serde_json::to_value(&body).unwrap()["IsPaused"], true);
    }

    #[test]
    fn empty_optional_ids_are_omitted() {
        let mut req = request("stop", false);
        req.media_source_id = Some("   ".into());
        req.play_session_id = None;
        let value = serde_json::to_value(ReportBody::new(&req, ReportEvent::Stop)).unwrap();
        assert!(value.get("MediaSourceId").is_none(), "{value}");
        assert!(value.get("PlaySessionId").is_none(), "{value}");
        assert_eq!(value["EventName"], "Stop");
    }

    #[test]
    fn negative_positions_are_clamped() {
        let mut req = request("progress", false);
        req.position_seconds = -10.0;
        let value = serde_json::to_value(ReportBody::new(&req, ReportEvent::Progress)).unwrap();
        assert_eq!(value["PositionTicks"], 0);
    }
}
