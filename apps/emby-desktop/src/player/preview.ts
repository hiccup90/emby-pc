import type { OverlayAction } from "../controls/contract";
import type { PlayerSnapshot, PlayerTrack } from "./contract";

/**
 * Browser-preview implementation of a control action. Both the main window's
 * `usePlayer` and the standalone controls window use this so the preview cannot
 * drift from the real player (loading pauses, ended restarts from zero, seek
 * leaves `ended`).
 */
export function applyPreviewAction(
  previous: PlayerSnapshot | null,
  action: OverlayAction,
): PlayerSnapshot | null {
  if (!previous) return previous;
  const next = { ...previous, seq: previous.seq + 1 };
  switch (action.type) {
    case "toggle-play":
      if (previous.status === "ended") {
        return { ...next, status: "playing", positionSeconds: 0, buffering: false };
      }
      return {
        ...next,
        status:
          previous.status === "playing" || previous.status === "loading"
            ? "paused"
            : "playing",
        buffering: false,
      };
    case "seek": {
      const positionSeconds = Math.min(
        Math.max(0, action.seconds),
        previous.durationSeconds,
      );
      return {
        ...next,
        positionSeconds,
        status: previous.status === "ended" ? "paused" : previous.status,
      };
    }
    case "set-volume":
      return {
        ...next,
        volume: Math.round(Math.min(100, Math.max(0, action.value))),
      };
    case "toggle-mute":
      return { ...next, muted: !previous.muted };
    case "set-track": {
      const mark = (track: PlayerTrack) => ({
        ...track,
        selected: action.id === "no" ? false : track.id === action.id,
      });
      return {
        ...next,
        audioTracks:
          action.kind === "audio"
            ? previous.audioTracks.map(mark)
            : previous.audioTracks,
        subtitleTracks:
          action.kind === "subtitle"
            ? previous.subtitleTracks.map(mark)
            : previous.subtitleTracks,
      };
    }
    default:
      return previous;
  }
}
