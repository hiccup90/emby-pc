import type { PlaybackIntro } from "../player/contract";

/**
 * Last intro range the server reported for an item, keyed by item id.
 *
 * Emby probes a strm media source the first time it is played and stores the
 * probe result over the item's chapters, which drops the `IntroStart` /
 * `IntroEnd` markers the server-side plugin wrote — from then on the server has
 * nothing left to skip. The player already saw the range, so it keeps it here
 * and falls back to it when a later launch comes back without markers (see
 * `playbackMeta`). Remove this once the server keeps markers across the probe.
 *
 * Best effort: unavailable storage only costs the fallback.
 */
const INTRO_KEY = "emby.intro.";

export function recallIntro(itemId: string): PlaybackIntro | null {
  try {
    const raw = window.localStorage.getItem(INTRO_KEY + itemId);
    if (!raw) return null;
    const value = JSON.parse(raw) as Partial<PlaybackIntro> | null;
    const startSeconds = value?.startSeconds;
    const endSeconds = value?.endSeconds;
    if (
      typeof startSeconds !== "number" ||
      typeof endSeconds !== "number" ||
      !Number.isFinite(startSeconds) ||
      !Number.isFinite(endSeconds) ||
      endSeconds <= startSeconds
    ) {
      return null;
    }
    return { startSeconds, endSeconds };
  } catch {
    return null;
  }
}

export function rememberIntro(itemId: string, intro: PlaybackIntro): void {
  const previous = recallIntro(itemId);
  if (
    previous &&
    previous.startSeconds === intro.startSeconds &&
    previous.endSeconds === intro.endSeconds
  ) {
    return;
  }
  try {
    window.localStorage.setItem(INTRO_KEY + itemId, JSON.stringify(intro));
  } catch {
    // Storage may be unavailable; the fallback is best effort.
  }
}
