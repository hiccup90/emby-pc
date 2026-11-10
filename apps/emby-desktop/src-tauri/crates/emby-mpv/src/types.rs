//! Wire types shared with the React frontend.
//!
//! Field names and JSON casing are a hard contract: the frontend was written
//! against these exact names. `#[serde(rename_all = "camelCase")]` is applied
//! to every struct, so `position_seconds` becomes `positionSeconds` on the wire.

pub const STATUS_IDLE: &str = "idle";
pub const STATUS_LOADING: &str = "loading";
pub const STATUS_PLAYING: &str = "playing";
pub const STATUS_PAUSED: &str = "paused";
pub const STATUS_ENDED: &str = "ended";
pub const STATUS_ERROR: &str = "error";

pub const TRACK_AUDIO: &str = "audio";
pub const TRACK_SUBTITLE: &str = "subtitle";

/// Playback statistics read from mpv properties for the player's info panel.
#[derive(Debug, Clone, Default, PartialEq)]
#[cfg_attr(feature = "serde", derive(serde::Serialize, serde::Deserialize))]
#[cfg_attr(feature = "serde", serde(rename_all = "camelCase"))]
pub struct PlayerStats {
    /// Active hardware decoder ("d3d11va"); None when decoding in software.
    pub hwdec: Option<String>,
    /// Active video output ("gpu-next", "gpu").
    pub vo: Option<String>,
    pub pixel_format: Option<String>,
    pub video_codec: Option<String>,
    /// Display refresh rate reported by the OS.
    pub display_fps: Option<f64>,
    /// Measured video frame rate.
    pub video_fps: Option<f64>,
    /// Dropped frames (renderer + decoder).
    pub dropped_frames: Option<i64>,
    /// Video bitrate in bits/s.
    pub bitrate: Option<f64>,
}

/// Snapshot of the player state, pushed over a Tauri `Channel<PlayerSnapshot>`.
#[derive(Debug, Clone, PartialEq)]
#[cfg_attr(feature = "serde", derive(serde::Serialize, serde::Deserialize))]
#[cfg_attr(feature = "serde", serde(rename_all = "camelCase"))]
pub struct PlayerSnapshot {
    pub seq: u64,
    /// One of [`STATUS_IDLE`], [`STATUS_LOADING`], [`STATUS_PLAYING`],
    /// [`STATUS_PAUSED`], [`STATUS_ENDED`], [`STATUS_ERROR`].
    pub status: String,
    pub position_seconds: f64,
    pub duration_seconds: f64,
    /// 0..100 (matches mpv's default `volume` property range).
    pub volume: f64,
    pub muted: bool,
    pub buffering: bool,
    pub video_width: i64,
    pub video_height: i64,
    pub audio_tracks: Vec<PlayerTrack>,
    pub subtitle_tracks: Vec<PlayerTrack>,
    pub error: Option<String>,
}

impl Default for PlayerSnapshot {
    fn default() -> Self {
        Self {
            seq: 0,
            status: STATUS_IDLE.to_string(),
            position_seconds: 0.0,
            duration_seconds: 0.0,
            volume: 100.0,
            muted: false,
            buffering: false,
            video_width: 0,
            video_height: 0,
            audio_tracks: Vec::new(),
            subtitle_tracks: Vec::new(),
            error: None,
        }
    }
}

/// One audio/subtitle track as exposed to the UI.
#[derive(Debug, Clone, PartialEq)]
#[cfg_attr(feature = "serde", derive(serde::Serialize, serde::Deserialize))]
#[cfg_attr(feature = "serde", serde(rename_all = "camelCase"))]
pub struct PlayerTrack {
    /// mpv track id (positive). `-1`/"no" means disabled.
    pub id: i64,
    /// `"audio"` or `"subtitle"`.
    pub r#type: String,
    pub title: String,
    pub lang: Option<String>,
    pub selected: bool,
}

impl PlayerTrack {
    pub fn new(id: i64, kind: TrackKind, title: String, lang: Option<String>, selected: bool) -> Self {
        Self {
            id,
            r#type: kind.as_str().to_string(),
            title,
            lang,
            selected,
        }
    }
}

/// Geometry hand-off from the web UI (CSS pixels + device pixel ratio).
#[derive(Debug, Clone, Copy, PartialEq)]
#[cfg_attr(feature = "serde", derive(serde::Serialize, serde::Deserialize))]
#[cfg_attr(feature = "serde", serde(rename_all = "camelCase"))]
pub struct VideoSync {
    pub seq: u64,
    pub visible: bool,
    pub x: f64,
    pub y: f64,
    pub w: f64,
    pub h: f64,
    pub dpr: f64,
}

impl VideoSync {
    pub fn hidden(seq: u64) -> Self {
        Self {
            seq,
            visible: false,
            x: 0.0,
            y: 0.0,
            w: 0.0,
            h: 0.0,
            dpr: 1.0,
        }
    }
}

/// `player_open` request.
#[derive(Debug, Clone, PartialEq)]
#[cfg_attr(feature = "serde", derive(serde::Serialize, serde::Deserialize))]
#[cfg_attr(feature = "serde", serde(rename_all = "camelCase"))]
pub struct OpenRequest {
    pub url: String,
    pub start_seconds: Option<f64>,
    /// Reserved for `http-header-fields`. libmpv only accepts that option
    /// before `mpv_initialize()`, so it is applied when a session is created
    /// for this request; later opens reuse the running session without it.
    pub headers: Option<Vec<String>>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum TrackKind {
    Audio,
    Subtitle,
}

impl TrackKind {
    pub fn as_str(self) -> &'static str {
        match self {
            TrackKind::Audio => TRACK_AUDIO,
            TrackKind::Subtitle => TRACK_SUBTITLE,
        }
    }

    /// mpv exposes subtitles as type `"sub"`, the UI contract uses `"subtitle"`.
    pub fn from_mpv(value: &str) -> Option<Self> {
        match value {
            "audio" => Some(TrackKind::Audio),
            "sub" | "subtitle" => Some(TrackKind::Subtitle),
            _ => None,
        }
    }

    pub fn parse(value: &str) -> Option<Self> {
        match value {
            TRACK_AUDIO => Some(TrackKind::Audio),
            TRACK_SUBTITLE | "sub" => Some(TrackKind::Subtitle),
            _ => None,
        }
    }

    /// mpv property used to select tracks (`aid` / `sid`).
    pub fn property(self) -> &'static str {
        match self {
            TrackKind::Audio => "aid",
            TrackKind::Subtitle => "sid",
        }
    }
}

/// Internal player state machine values.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PlayerStatus {
    Idle,
    Loading,
    Playing,
    Paused,
    Ended,
    Error,
}

impl PlayerStatus {
    pub fn as_str(self) -> &'static str {
        match self {
            PlayerStatus::Idle => STATUS_IDLE,
            PlayerStatus::Loading => STATUS_LOADING,
            PlayerStatus::Playing => STATUS_PLAYING,
            PlayerStatus::Paused => STATUS_PAUSED,
            PlayerStatus::Ended => STATUS_ENDED,
            PlayerStatus::Error => STATUS_ERROR,
        }
    }
}

impl From<PlayerStatus> for String {
    fn from(status: PlayerStatus) -> Self {
        status.as_str().to_string()
    }
}

/// Commands delivered to the dedicated mpv thread over `std::sync::mpsc`.
#[derive(Debug)]
pub enum PlayerCommand {
    Open {
        url: String,
        start_seconds: Option<f64>,
        /// Replies with the first snapshot published for this open, so
        /// `player_open` can return a `seq`-consistent loading state instead of
        /// racing the worker's property events.
        reply: Option<std::sync::mpsc::Sender<PlayerSnapshot>>,
    },
    SetPaused(bool),
    Seek {
        seconds: f64,
        exact: bool,
    },
    SetVolume(f64),
    SetMuted(bool),
    SetTrack {
        kind: TrackKind,
        id: i64,
    },
    Stop,
    /// Ask the worker to publish a snapshot to `reply`.
    Snapshot(std::sync::mpsc::Sender<PlayerSnapshot>),
    /// Ask the worker for the current mpv statistics.
    Stats(std::sync::mpsc::Sender<PlayerStats>),
    /// Quit mpv gracefully, then terminate the handle.
    Shutdown,
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    #[cfg(feature = "serde-tests")]
    fn snapshot_serializes_with_camel_case_keys() {
        let mut snapshot = PlayerSnapshot::default();
        snapshot.seq = 7;
        snapshot.status = STATUS_LOADING.to_string();
        snapshot.position_seconds = 12.5;
        snapshot.duration_seconds = 600.0;
        snapshot.video_width = 3840;
        snapshot.video_height = 2160;
        snapshot.audio_tracks.push(PlayerTrack::new(
            1,
            TrackKind::Audio,
            "TrueHD 7.1".to_string(),
            Some("eng".to_string()),
            true,
        ));
        snapshot.subtitle_tracks.push(PlayerTrack::new(
            2,
            TrackKind::Subtitle,
            "Forced".to_string(),
            None,
            false,
        ));
        snapshot.error = Some("boom".to_string());

        let json = serde_json::to_value(&snapshot).unwrap();
        for key in [
            "seq",
            "status",
            "positionSeconds",
            "durationSeconds",
            "volume",
            "muted",
            "buffering",
            "videoWidth",
            "videoHeight",
            "audioTracks",
            "subtitleTracks",
            "error",
        ] {
            assert!(json.get(key).is_some(), "missing key {key}: {json}");
        }
        assert!(json.get("position_seconds").is_none());
        assert_eq!(json["audioTracks"][0]["type"], "audio");
        assert_eq!(json["subtitleTracks"][0]["type"], "subtitle");
        assert_eq!(json["audioTracks"][0]["lang"], "eng");
        assert!(json["subtitleTracks"][0]["lang"].is_null());
    }

    #[test]
    #[cfg(feature = "serde-tests")]
    fn video_sync_deserializes_from_frontend_camel_case() {
        let sync: VideoSync = serde_json::from_str(
            r#"{"seq":42,"visible":true,"x":10.5,"y":20.0,"w":640.0,"h":360.0,"dpr":1.5}"#,
        )
        .unwrap();
        assert_eq!(sync.seq, 42);
        assert!(sync.visible);
        assert!((sync.x - 10.5).abs() < f64::EPSILON);
        assert!((sync.dpr - 1.5).abs() < f64::EPSILON);
    }

    #[test]
    #[cfg(feature = "serde-tests")]
    fn open_request_accepts_missing_optional_fields() {
        let req: OpenRequest =
            serde_json::from_str(r#"{"url":"https://example/media/1.mkv"}"#).unwrap();
        assert_eq!(req.url, "https://example/media/1.mkv");
        assert_eq!(req.start_seconds, None);
        assert_eq!(req.headers, None);
    }

    #[test]
    fn track_kind_maps_mpv_sub_type() {
        assert_eq!(TrackKind::from_mpv("sub"), Some(TrackKind::Subtitle));
        assert_eq!(TrackKind::from_mpv("audio"), Some(TrackKind::Audio));
        assert_eq!(TrackKind::from_mpv("video"), None);
        assert_eq!(TrackKind::Audio.property(), "aid");
        assert_eq!(TrackKind::Subtitle.property(), "sid");
        assert_eq!(TrackKind::Subtitle.as_str(), "subtitle");
    }

    #[test]
    fn status_strings_match_contract() {
        let all = [
            PlayerStatus::Idle,
            PlayerStatus::Loading,
            PlayerStatus::Playing,
            PlayerStatus::Paused,
            PlayerStatus::Ended,
            PlayerStatus::Error,
        ];
        assert_eq!(
            all.map(PlayerStatus::as_str),
            ["idle", "loading", "playing", "paused", "ended", "error"]
        );
    }

    /// Linker-free guard: the camelCase attribute is the wire contract for the
    /// React frontend. This test runs even when the `serde` feature is off
    /// (this repository's CI container cannot link serde's build script), so
    /// accidentally deleting the attribute is still caught.
    #[test]
    fn wire_types_keep_camel_case_serde_contract() {
        let source = include_str!("types.rs");
        let rename = "#[cfg_attr(feature = \"serde\", serde(rename_all = \"camelCase\"))]";
        let derive = "#[cfg_attr(feature = \"serde\", derive(serde::Serialize, serde::Deserialize))]";
        for struct_name in [
            "pub struct PlayerStats",
            "pub struct PlayerSnapshot",
            "pub struct PlayerTrack",
            "pub struct VideoSync",
            "pub struct OpenRequest",
        ] {
            let position = source
                .find(struct_name)
                .unwrap_or_else(|| panic!("{struct_name} not found"));
            let preceding: Vec<&str> = source[..position]
                .lines()
                .rev()
                .take(4)
                .map(str::trim)
                .collect();
            assert!(
                preceding.contains(&rename),
                "{struct_name} lost serde camelCase rename"
            );
            assert!(
                preceding.contains(&derive),
                "{struct_name} lost serde derive"
            );
        }
    }
}
