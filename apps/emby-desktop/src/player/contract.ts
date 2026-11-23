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

/** A normal chapter shown in the player's chapter menu. */
export interface PlaybackChapter {
  seconds: number;
  title: string;
}

/** Intro segment derived from Emby's `IntroStart` / `IntroEnd` markers. */
export interface PlaybackIntro {
  startSeconds: number;
  endSeconds: number;
}

/** How the player handles an available intro segment. */
export type SkipIntroMode = "off" | "button" | "auto";

/** What the player shows about the current item (title row of the controls). */
export interface PlaybackMeta {
  /** Movie title, or the series name for an episode. */
  title: string;
  /** Secondary line: the year for movies, "S1 · E3 — 集名" for episodes. */
  subtitle: string | null;
  /** Normal chapters for the chapter menu; marker rows are filtered out. */
  chapters?: PlaybackChapter[];
  /** Intro segment from Emby markers; absent when the item has none. */
  intro?: PlaybackIntro;
  /**
   * True only in button mode. Auto mode still carries `intro` (the player
   * uses it to seek) but the controls must not render a second skip button.
   */
  skipIntroButton?: boolean;
  /**
   * Emby image tag for the item's own transparent `Logo`, copied from
   * `imageTags.Logo` when the item has one. Absent means "no logo": the
   * player must not ask the server for an image in that case.
   */
  logoTag?: string | null;
  /** Episode without its own logo: the series whose logo may be used instead. */
  logoSeriesId?: string | null;
  /** Resolved logo URL; null/absent falls back to the plain title text. */
  logoUrl?: string | null;
  /**
   * Episode navigation, computed by the shell from its episode run and put on
   * the wire by the player page. Both absent means "not an episode" (a movie
   * has no previous/next neighbour), so the controls show no such buttons.
   */
  hasPrevEpisode?: boolean;
  hasNextEpisode?: boolean;
}

export interface OpenRequest {
  url: string;
  startSeconds?: number | null;
}
