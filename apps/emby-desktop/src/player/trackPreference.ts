export interface TrackChoice {
  lang: string | null;
  title: string;
}

/** Remembered per-series track choice (the ids change per file). */
export interface TrackPreference {
  audio?: TrackChoice;
  /** null means the user explicitly turned subtitles off. */
  subtitle?: TrackChoice | null;
}

const TRACK_PREF_KEY = "emby.track-preference.";

export function readTrackPreference(scope: string): TrackPreference | null {
  try {
    const raw = window.localStorage.getItem(TRACK_PREF_KEY + scope);
    if (!raw) return null;
    const value = JSON.parse(raw) as unknown;
    return value && typeof value === "object" ? (value as TrackPreference) : null;
  } catch {
    return null;
  }
}

export function writeTrackPreference(scope: string, value: TrackPreference) {
  try {
    window.localStorage.setItem(TRACK_PREF_KEY + scope, JSON.stringify(value));
  } catch {
    // Storage may be unavailable; remembering the choice is best effort.
  }
}

/** Labels differ between Emby streams and mpv ("国语 AAC 5.1" vs "国语 · AAC 5.1"). */
function normalizeTrackText(value: string | null | undefined): string {
  return (value ?? "")
    .toLowerCase()
    .replace(/[（(][^）)]*[）)]/g, "")
    .replace(/[^\p{L}\p{N}]+/gu, "");
}

/**
 * Find the track matching a remembered choice. The id is not stable across
 * files, so match the label first and fall back to a unique language.
 */
export function findTrackIndexByChoice<T extends { lang: string | null; title: string }>(
  tracks: readonly T[],
  choice: TrackChoice,
): number {
  const wantedTitle = normalizeTrackText(choice.title);
  if (wantedTitle) {
    const exact = tracks.findIndex(
      (track) =>
        (track.lang ?? null) === (choice.lang ?? null) &&
        normalizeTrackText(track.title) === wantedTitle,
    );
    if (exact >= 0) return exact;
    const titleMatches = tracks
      .map((track, index) => ({ track, index }))
      .filter(({ track }) => normalizeTrackText(track.title) === wantedTitle);
    if (titleMatches.length === 1) return titleMatches[0].index;
  }
  const wantedLang = choice.lang ?? null;
  if (wantedLang) {
    const langMatches = tracks
      .map((track, index) => ({ track, index }))
      .filter(({ track }) => (track.lang ?? null) === wantedLang);
    if (langMatches.length === 1) return langMatches[0].index;
  }
  return -1;
}
