import {
  describeEmbyError,
  embyReportPlayback,
  hasTauriRuntime,
} from "./client";
import type { EmbyPlaybackEvent } from "./client";
import type { EmbyPlaybackTarget } from "./contract";

export function reportEmbyPlayback(
  target: EmbyPlaybackTarget | null,
  event: EmbyPlaybackEvent,
  positionSeconds: number,
  paused: boolean,
): void {
  if (!target || !hasTauriRuntime()) return;

  const safePosition = Number.isFinite(positionSeconds)
    ? Math.max(0, Math.round(positionSeconds))
    : 0;

  void embyReportPlayback({
    itemId: target.itemId,
    ...(target.mediaSourceId ? { mediaSourceId: target.mediaSourceId } : {}),
    ...(target.playSessionId ? { playSessionId: target.playSessionId } : {}),
    positionSeconds: safePosition,
    paused,
    event,
  }).catch((reason: unknown) => {
    console.warn(
      `[emby] playback ${event} report failed: ${describeEmbyError(reason, "unknown error")}`,
    );
  });
}
