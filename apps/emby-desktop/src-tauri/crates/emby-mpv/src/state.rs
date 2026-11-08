//! Pure state machine translating mpv property/event changes into
//! `PlayerSnapshot`s. No FFI here, so it is fully unit tested.

use crate::types::{PlayerSnapshot, PlayerStatus, PlayerTrack};

/// Highest `position` we trust before clamping to `duration`.
const POSITION_EPSILON: f64 = 0.05;

#[derive(Debug)]
pub struct SnapshotAssembler {
    status: PlayerStatus,
    paused: bool,
    position: f64,
    duration: f64,
    volume: f64,
    muted: bool,
    buffering: bool,
    video_width: i64,
    video_height: i64,
    audio_tracks: Vec<PlayerTrack>,
    subtitle_tracks: Vec<PlayerTrack>,
    error: Option<String>,
    pending_seek: Option<f64>,
    dirty: bool,
}

impl Default for SnapshotAssembler {
    fn default() -> Self {
        Self::new()
    }
}

impl SnapshotAssembler {
    pub fn new() -> Self {
        Self {
            status: PlayerStatus::Idle,
            paused: false,
            position: 0.0,
            duration: 0.0,
            volume: 100.0,
            muted: false,
            buffering: false,
            video_width: 0,
            video_height: 0,
            audio_tracks: Vec::new(),
            subtitle_tracks: Vec::new(),
            error: None,
            pending_seek: None,
            dirty: false,
        }
    }

    pub fn status(&self) -> PlayerStatus {
        self.status
    }

    pub fn is_playing(&self) -> bool {
        self.status == PlayerStatus::Playing
    }

    pub fn is_dirty(&self) -> bool {
        self.dirty
    }

    pub fn video_width(&self) -> i64 {
        self.video_width
    }

    pub fn video_height(&self) -> i64 {
        self.video_height
    }

    pub fn mark_dirty(&mut self) {
        self.dirty = true;
    }

    pub fn clear_dirty(&mut self) {
        self.dirty = false;
    }

    pub fn set_volume(&mut self, volume: f64) {
        if volume.is_finite() && (volume - self.volume).abs() > f64::EPSILON {
            self.volume = volume.clamp(0.0, 100.0);
            self.dirty = true;
        }
    }

    pub fn set_muted(&mut self, muted: bool) {
        if muted != self.muted {
            self.muted = muted;
            self.dirty = true;
        }
    }

    pub fn set_buffering(&mut self, buffering: bool) {
        if buffering != self.buffering {
            self.buffering = buffering;
            self.dirty = true;
        }
    }

    pub fn set_paused(&mut self, paused: bool) {
        if paused == self.paused {
            return;
        }
        self.paused = paused;
        match self.status {
            PlayerStatus::Playing => self.status = PlayerStatus::Paused,
            PlayerStatus::Paused => {
                if !paused {
                    self.status = PlayerStatus::Playing
                }
            }
            _ => {}
        }
        self.dirty = true;
    }

    pub fn set_position(&mut self, position: f64) {
        if !position.is_finite() || position < 0.0 {
            return;
        }
        // `ended` stays parked at duration (see contract); `idle` must stay at
        // 0 even when a time-pos event was already queued before `stop`.
        if matches!(
            self.status,
            PlayerStatus::Idle | PlayerStatus::Ended | PlayerStatus::Error
        ) {
            return;
        }
        self.position = position;
        // No dirty flag: time-pos changes at 20-60 Hz and is published on the
        // 5 Hz tick instead of once per property event.
    }

    pub fn set_duration(&mut self, duration: f64) {
        if !duration.is_finite() || duration < 0.0 {
            return;
        }
        // Same stale-event protection as `set_position`.
        if self.status == PlayerStatus::Idle {
            return;
        }
        if (duration - self.duration).abs() > f64::EPSILON {
            self.duration = duration;
            self.dirty = true;
        }
    }

    pub fn set_video_size(&mut self, width: i64, height: i64) {
        if self.status == PlayerStatus::Idle {
            return;
        }
        let width = width.max(0);
        let height = height.max(0);
        if width != self.video_width || height != self.video_height {
            self.video_width = width;
            self.video_height = height;
            self.dirty = true;
        }
    }

    pub fn set_tracks(&mut self, audio: Vec<PlayerTrack>, subtitles: Vec<PlayerTrack>) {
        if audio != self.audio_tracks || subtitles != self.subtitle_tracks {
            self.audio_tracks = audio;
            self.subtitle_tracks = subtitles;
            self.dirty = true;
        }
    }

    pub fn begin_open(&mut self, start_seconds: Option<f64>) {
        let start = start_seconds.filter(|s| s.is_finite() && *s > 0.0).unwrap_or(0.0);
        self.status = PlayerStatus::Loading;
        self.paused = false;
        self.position = start;
        self.duration = 0.0;
        self.buffering = false;
        self.error = None;
        self.pending_seek = if start > 0.0 { Some(start) } else { None };
        self.video_width = 0;
        self.video_height = 0;
        self.audio_tracks.clear();
        self.subtitle_tracks.clear();
        self.dirty = true;
    }

    pub fn take_pending_seek(&mut self) -> Option<f64> {
        self.pending_seek.take()
    }

    pub fn set_pending_seek(&mut self, seconds: f64) {
        if seconds.is_finite() && seconds >= 0.0 {
            self.pending_seek = Some(seconds);
        }
    }

    pub fn on_file_loaded(&mut self) {
        // A `stop` command can be processed while a `loadfile` is still in
        // flight; the late FILE_LOADED must not resurrect an idle session.
        // Legitimate loads always arrive in `Loading` (set by `begin_open`).
        if matches!(self.status, PlayerStatus::Idle | PlayerStatus::Ended) {
            return;
        }
        self.status = if self.paused {
            PlayerStatus::Paused
        } else {
            PlayerStatus::Playing
        };
        self.buffering = false;
        self.dirty = true;
    }

    /// `MPV_EVENT_END_FILE`.
    ///
    /// While `Loading`, mpv is replacing the previous playlist entry: its
    /// END_FILE(STOP/EOF) must not knock the assembler out of `loading` or
    /// drop the pending `start_seconds` seek. Only a real error is reported.
    pub fn on_end_file(&mut self, reason: i32, error: Option<String>) {
        if self.status == PlayerStatus::Loading
            && reason != crate::ffi::MPV_END_FILE_REASON_ERROR
        {
            return;
        }
        match reason {
            crate::ffi::MPV_END_FILE_REASON_EOF => self.finish(),
            crate::ffi::MPV_END_FILE_REASON_STOP | crate::ffi::MPV_END_FILE_REASON_QUIT => {
                self.reset_to_idle()
            }
            crate::ffi::MPV_END_FILE_REASON_ERROR => {
                self.status = PlayerStatus::Error;
                self.buffering = false;
                self.error = Some(error.unwrap_or_else(|| "playback failed".to_string()));
                self.dirty = true;
            }
            // REDIRECT (playlist expansion) and unknown reasons: keep playing.
            _ => {}
        }
    }

    /// `eof-reached` property.
    pub fn set_eof_reached(&mut self, reached: bool) {
        if reached {
            // Ignore a queued EOF notification from the file we just replaced.
            if matches!(self.status, PlayerStatus::Loading | PlayerStatus::Idle) {
                return;
            }
            self.finish();
        } else if self.status == PlayerStatus::Ended {
            self.status = if self.paused {
                PlayerStatus::Paused
            } else {
                PlayerStatus::Playing
            };
            self.dirty = true;
        }
    }

    pub fn set_idle_active(&mut self, idle: bool) {
        if !idle {
            return;
        }
        // Never clobber a terminal or in-progress state: mpv reports
        // idle-active after an END_FILE error too, keep-open may unload right
        // after EOF, and a queued notification from the replaced file must not
        // clear the pending seek of an open that is still loading.
        if matches!(
            self.status,
            PlayerStatus::Ended | PlayerStatus::Error | PlayerStatus::Loading
        ) {
            return;
        }
        if self.status != PlayerStatus::Idle {
            self.reset_to_idle();
        }
    }

    /// Explicit `player_stop` / `stop` command.
    pub fn on_stopped(&mut self) {
        self.reset_to_idle();
    }

    pub fn set_error(&mut self, message: impl Into<String>) {
        self.status = PlayerStatus::Error;
        self.buffering = false;
        self.error = Some(message.into());
        self.dirty = true;
    }

    fn finish(&mut self) {
        if self.status == PlayerStatus::Ended && self.error.is_none() {
            return;
        }
        self.status = PlayerStatus::Ended;
        self.buffering = false;
        self.error = None;
        self.dirty = true;
    }

    fn reset_to_idle(&mut self) {
        self.status = PlayerStatus::Idle;
        self.paused = false;
        self.position = 0.0;
        self.duration = 0.0;
        self.buffering = false;
        self.error = None;
        self.pending_seek = None;
        self.video_width = 0;
        self.video_height = 0;
        self.audio_tracks.clear();
        self.subtitle_tracks.clear();
        self.dirty = true;
    }

    pub fn build(&self) -> PlayerSnapshot {
        let position = if self.status == PlayerStatus::Ended {
            if self.duration > 0.0 {
                self.duration
            } else {
                self.position
            }
        } else if self.duration > 0.0 {
            self.position.clamp(0.0, self.duration + POSITION_EPSILON)
        } else {
            self.position.max(0.0)
        };

        PlayerSnapshot {
            seq: 0, // assigned by SnapshotHub
            status: self.status.as_str().to_string(),
            position_seconds: position,
            duration_seconds: self.duration,
            volume: self.volume,
            muted: self.muted,
            buffering: self.buffering,
            video_width: self.video_width,
            video_height: self.video_height,
            audio_tracks: self.audio_tracks.clone(),
            subtitle_tracks: self.subtitle_tracks.clone(),
            error: self.error.clone(),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::types::{TrackKind, STATUS_ENDED, STATUS_ERROR, STATUS_IDLE, STATUS_LOADING, STATUS_PAUSED, STATUS_PLAYING};

    fn asm() -> SnapshotAssembler {
        let mut asm = SnapshotAssembler::new();
        asm.begin_open(None);
        asm.on_file_loaded();
        asm
    }

    #[test]
    fn idle_to_loading_to_playing() {
        let mut asm = SnapshotAssembler::new();
        assert_eq!(asm.status(), PlayerStatus::Idle);
        asm.begin_open(None);
        assert_eq!(asm.build().status, STATUS_LOADING);
        assert!(asm.is_dirty());
        asm.clear_dirty();
        asm.set_duration(600.0);
        assert!(asm.is_dirty(), "duration change must mark dirty");
        asm.clear_dirty();
        asm.set_position(1.0);
        assert!(!asm.is_dirty(), "time-pos must not mark dirty");
        asm.on_file_loaded();
        assert_eq!(asm.build().status, STATUS_PLAYING);
        assert!(asm.is_playing());
        assert_eq!(asm.build().position_seconds, 1.0);
    }

    #[test]
    fn pause_toggles_playing_and_paused() {
        let mut asm = asm();
        asm.set_paused(true);
        assert_eq!(asm.build().status, STATUS_PAUSED);
        asm.set_paused(false);
        assert_eq!(asm.build().status, STATUS_PLAYING);
        // A pause report while loading must not change the status.
        let mut loading = SnapshotAssembler::new();
        loading.begin_open(None);
        loading.set_paused(true);
        assert_eq!(loading.build().status, STATUS_LOADING);
    }

    #[test]
    fn eof_parks_position_at_duration() {
        let mut asm = asm();
        asm.set_duration(120.0);
        asm.set_position(119.0);
        asm.set_eof_reached(true);
        let snap = asm.build();
        assert_eq!(snap.status, STATUS_ENDED);
        assert_eq!(snap.position_seconds, 120.0);
        assert_eq!(snap.duration_seconds, 120.0);

        // Later time-pos updates (keep-open) must not move the parked position.
        asm.set_position(30.0);
        assert_eq!(asm.build().position_seconds, 120.0);

        // Seeking back re-opens playback.
        asm.set_eof_reached(false);
        assert_eq!(asm.build().status, STATUS_PLAYING);
    }

    #[test]
    fn end_file_eof_also_reports_ended() {
        let mut asm = asm();
        asm.set_duration(42.0);
        asm.on_end_file(crate::ffi::MPV_END_FILE_REASON_EOF, None);
        let snap = asm.build();
        assert_eq!(snap.status, STATUS_ENDED);
        assert_eq!(snap.position_seconds, 42.0);
    }

    #[test]
    fn end_file_error_reports_message_and_survives_idle() {
        let mut asm = asm();
        asm.on_end_file(
            crate::ffi::MPV_END_FILE_REASON_ERROR,
            Some("Failed to open https://x/y.mkv".to_string()),
        );
        let snap = asm.build();
        assert_eq!(snap.status, STATUS_ERROR);
        assert_eq!(snap.error.as_deref(), Some("Failed to open https://x/y.mkv"));

        // mpv unloads the file after a failed open -> idle-active must not
        // hide the error from the UI.
        asm.set_idle_active(true);
        assert_eq!(asm.build().status, STATUS_ERROR);
        assert!(asm.build().error.is_some());
    }

    #[test]
    fn end_file_stop_returns_to_idle() {
        let mut asm = asm();
        asm.set_duration(10.0);
        asm.set_position(5.0);
        asm.on_end_file(crate::ffi::MPV_END_FILE_REASON_STOP, None);
        let snap = asm.build();
        assert_eq!(snap.status, STATUS_IDLE);
        assert_eq!(snap.position_seconds, 0.0);
        assert_eq!(snap.duration_seconds, 0.0);
    }

    #[test]
    fn stop_command_resets_everything() {
        let mut asm = asm();
        asm.set_tracks(
            vec![PlayerTrack::new(1, TrackKind::Audio, "a".into(), None, true)],
            vec![PlayerTrack::new(2, TrackKind::Subtitle, "s".into(), Some("eng".into()), true)],
        );
        asm.set_position(42.0);
        asm.set_duration(90.0);
        asm.set_video_size(1280, 720);
        asm.on_stopped();
        let snap = asm.build();
        assert_eq!(snap.status, STATUS_IDLE);
        assert_eq!(snap.position_seconds, 0.0);
        assert_eq!(snap.duration_seconds, 0.0);
        assert_eq!((snap.video_width, snap.video_height), (0, 0));
        assert!(snap.audio_tracks.is_empty());
        assert!(snap.subtitle_tracks.is_empty());
        assert!(snap.error.is_none());
    }

    #[test]
    fn stale_properties_after_stop_cannot_revive_the_snapshot() {
        let mut asm = asm();
        asm.set_duration(100.0);
        asm.set_position(50.0);
        asm.set_video_size(1920, 1080);
        asm.on_stopped();

        // Property events that were already queued when `stop` ran.
        asm.set_position(50.0);
        asm.set_duration(100.0);
        asm.set_video_size(1920, 1080);
        asm.set_paused(false);

        let snap = asm.build();
        assert_eq!(snap.status, STATUS_IDLE);
        assert_eq!(snap.position_seconds, 0.0);
        assert_eq!(snap.duration_seconds, 0.0);
        assert_eq!((snap.video_width, snap.video_height), (0, 0));
    }

    #[test]
    fn a_late_file_loaded_after_stop_is_ignored() {
        let mut asm = SnapshotAssembler::new();
        asm.begin_open(Some(10.0));
        asm.on_stopped();
        asm.on_file_loaded();
        assert_eq!(asm.status(), PlayerStatus::Idle);
        assert_eq!(asm.take_pending_seek(), None);
    }

    #[test]
    fn idle_active_does_not_reset_ended_state() {
        let mut asm = asm();
        asm.set_duration(10.0);
        asm.set_eof_reached(true);
        asm.set_idle_active(true);
        assert_eq!(asm.build().status, STATUS_ENDED);
    }

    #[test]
    fn duration_change_marks_dirty_and_clamps_position() {
        let mut asm = asm();
        asm.set_duration(100.0);
        asm.set_position(100.03);
        assert_eq!(asm.build().position_seconds, 100.03);
        asm.set_position(100.5);
        assert!(asm.build().position_seconds <= 100.05);
        assert!(asm.is_dirty(), "duration change must mark dirty");
    }

    #[test]
    fn volume_and_mute_state() {
        let mut asm = SnapshotAssembler::new();
        asm.set_volume(150.0);
        assert_eq!(asm.build().volume, 100.0);
        asm.set_volume(-5.0);
        assert_eq!(asm.build().volume, 0.0);
        asm.set_muted(true);
        assert!(asm.build().muted);
        asm.set_volume(f64::NAN);
        assert_eq!(asm.build().volume, 0.0, "NaN volume is ignored");
    }

    #[test]
    fn pending_seek_from_open_request_is_applied_once() {
        let mut asm = SnapshotAssembler::new();
        asm.begin_open(Some(90.0));
        assert_eq!(asm.build().position_seconds, 90.0);
        assert_eq!(asm.take_pending_seek(), Some(90.0));
        assert_eq!(asm.take_pending_seek(), None);

        let mut negative = SnapshotAssembler::new();
        negative.begin_open(Some(-10.0));
        assert_eq!(negative.build().position_seconds, 0.0);
        assert_eq!(negative.take_pending_seek(), None);
    }

    #[test]
    fn replacing_the_current_file_keeps_the_pending_start_seek() {
        let mut asm = SnapshotAssembler::new();
        asm.begin_open(Some(120.0));
        // mpv stops the previous playlist entry while the new one loads.
        asm.on_end_file(crate::ffi::MPV_END_FILE_REASON_STOP, None);
        assert_eq!(asm.status(), PlayerStatus::Loading);
        assert_eq!(asm.take_pending_seek(), Some(120.0));

        // A queued eof/idle notification from the old file is ignored too.
        asm.set_eof_reached(true);
        asm.set_idle_active(true);
        assert_eq!(asm.status(), PlayerStatus::Loading);
        assert_eq!(asm.take_pending_seek(), None);
    }

    #[test]
    fn an_error_while_loading_still_reports_error() {
        let mut asm = SnapshotAssembler::new();
        asm.begin_open(Some(30.0));
        asm.on_end_file(
            crate::ffi::MPV_END_FILE_REASON_ERROR,
            Some("playback failed: loading failed".to_string()),
        );
        assert_eq!(asm.status(), PlayerStatus::Error);
        assert!(asm.build().error.is_some());
        // The UI contract for `error` keeps the loading position for context.
        assert_eq!(asm.build().position_seconds, 30.0);
    }

    #[test]
    fn buffering_flag_is_dirty_only_on_transition() {
        let mut asm = asm();
        asm.clear_dirty();
        asm.set_buffering(true);
        assert!(asm.is_dirty());
        asm.clear_dirty();
        asm.set_buffering(true);
        assert!(!asm.is_dirty());
    }
}
