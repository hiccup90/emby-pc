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

export interface VideoSync {
  seq: number;
  visible: boolean;
  x: number;
  y: number;
  w: number;
  h: number;
  dpr: number;
}

/** What the player shows about the current item (title row of the controls). */
export interface PlaybackMeta {
  /** Movie title, or the episode name. */
  title: string;
  /** Secondary line: the year for movies, "剧名 · S1 · E3" for episodes. */
  subtitle: string | null;
}

export interface OpenRequest {
  url: string;
  startSeconds?: number | null;
  headers?: string[] | null;
}
