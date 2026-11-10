export type PlayerStatus = "idle" | "loading" | "playing" | "paused" | "ended" | "error";

export interface PlayerTrack {
  id: number;
  type: "audio" | "subtitle";
  title: string;
  lang: string | null;
  selected: boolean;
}

export interface PlayerSnapshot {
  seq: number;
  status: PlayerStatus;
  positionSeconds: number;
  durationSeconds: number;
  volume: number;
  muted: boolean;
  buffering: boolean;
  videoWidth: number;
  videoHeight: number;
  audioTracks: PlayerTrack[];
  subtitleTracks: PlayerTrack[];
  error: string | null;
}

/** Live mpv statistics for the player's info panel. */
export interface PlayerStats {
  hwdec: string | null;
  vo: string | null;
  pixelFormat: string | null;
  videoCodec: string | null;
  displayFps: number | null;
  videoFps: number | null;
  droppedFrames: number | null;
  bitrate: number | null;
}

export interface VideoSync {
  seq: number;
  visible: boolean;
  x: number;
  y: number;
  w: number;
  h: number;
  dpr: number;
}

/** A chapter marker shown in the player's chapter menu. */
export interface PlaybackChapter {
  seconds: number;
  title: string;
}

/** What the player shows about the current item (title row of the controls). */
export interface PlaybackMeta {
  /** Movie title, or the series name for an episode. */
  title: string;
  /** Secondary line: the year for movies, "S1 · E3 — 集名" for episodes. */
  subtitle: string | null;
  /** Chapter markers; empty or absent when the item has none. */
  chapters?: PlaybackChapter[];
}

export interface OpenRequest {
  url: string;
  startSeconds?: number | null;
  headers?: string[] | null;
}
