import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import type { PointerEvent as ReactPointerEvent } from "react";
import { emitTo, listen } from "@tauri-apps/api/event";
import { getCurrentWindow } from "@tauri-apps/api/window";
import {
  invokePlayerUiMode,
  onOverlayRequest,
  publishOverlayState,
} from "../controls/bridge";
import type { OverlayAction, OverlayStateInput } from "../controls/contract";
import type { EmbyPlaybackTarget } from "../emby/contract";
import { embyImageUrl, embyItem } from "../emby/client";
import { reportEmbyPlayback } from "../emby/playback";
import type { PlaybackMeta, PlayerStats, PlayerStatus } from "./contract";
import { invokePlayerStats, invokePlayerStop } from "./tauri";
import { usePlayer } from "./usePlayer";
import { useVideoSync } from "./useVideoSync";

/** Coalescing window for position-only overlay updates (mpv reports several per second). */
const OVERLAY_STATE_THROTTLE_MS = 250;
/** Wide enough for the control bar's logo at 4K; the file is a transparent PNG. */
const LOGO_MAX_WIDTH = 520;

/**
 * Series id -> its `imageTags.Logo` (or null when it has none). Only episodes
 * without their own logo pay one request per series, and only once: the promise
 * is cached too, so StrictMode's double effect cannot fire two requests.
 */
const seriesLogoTagCache = new Map<string, Promise<string | null>>();

function seriesLogoTag(seriesId: string): Promise<string | null> {
  const cached = seriesLogoTagCache.get(seriesId);
  if (cached) return cached;
  const pending = embyItem(seriesId).then((item) => item.imageTags.Logo ?? null);
  seriesLogoTagCache.set(seriesId, pending);
  pending.catch(() => seriesLogoTagCache.delete(seriesId));
  return pending;
}

/** Built logo URLs, keyed by item + tag; the command itself is an IPC round-trip. */
const logoUrlCache = new Map<string, Promise<string | null>>();

function buildLogoUrl(itemId: string, tag: string): Promise<string | null> {
  const key = `${itemId}:${tag}`;
  const cached = logoUrlCache.get(key);
  if (cached) return cached;
  const pending = embyImageUrl({
    itemId,
    imageType: "Logo",
    maxWidth: LOGO_MAX_WIDTH,
    tag,
  })
    .then((url) => (url.trim() ? url : null))
    .catch((reason: unknown) => {
      console.warn("[player] could not build the logo URL", reason);
      return null;
    });
  logoUrlCache.set(key, pending);
  return pending;
}
/** Info-panel statistics cadence; runs only while that panel is open. */
const STATS_POLL_MS = 1500;

function statusLabel(status: PlayerStatus): string {
  const labels: Record<PlayerStatus, string> = {
    idle: "尚未载入媒体",
    loading: "正在载入",
    playing: "播放中",
    paused: "已暂停",
    ended: "播放结束",
    error: "播放错误",
  };
  return labels[status];
}

interface PlayerPageProps {
  playback?: EmbyPlaybackTarget;
  /** Title row for the controls overlay. */
  meta?: PlaybackMeta;
  /** Series (or movie) id; keys the remembered audio/subtitle choice. */
  trackScope?: string | null;
  /** Start the next queued item; resolves false when there is none. */
  onNext?: () => Promise<boolean>;
  onBack: () => void;
}

interface TrackChoice {
  lang: string | null;
  title: string;
}

/** Remembered per-series track choice (the ids change per file). */
interface TrackPreference {
  audio?: TrackChoice;
  subtitle?: TrackChoice;
}

const TRACK_PREF_KEY = "emby.track-preference.";

function readTrackPreference(scope: string): TrackPreference | null {
  try {
    const raw = window.localStorage.getItem(TRACK_PREF_KEY + scope);
    if (!raw) return null;
    const value = JSON.parse(raw) as TrackPreference;
    return value && typeof value === "object" ? value : null;
  } catch {
    return null;
  }
}

function writeTrackPreference(scope: string, value: TrackPreference) {
  try {
    window.localStorage.setItem(TRACK_PREF_KEY + scope, JSON.stringify(value));
  } catch {
    // Storage may be unavailable; remembering the choice is best effort.
  }
}

function sameChoice(track: { lang: string | null; title: string }, choice: TrackChoice): boolean {
  return (track.lang ?? null) === (choice.lang ?? null) && track.title === choice.title;
}

/**
 * The playback page is intentionally chrome-free: the native mpv window covers
 * the webview, so every control lives in the transparent `controls.html`
 * overlay window and talks back through `overlay-action`.
 */
export default function PlayerPage({
  playback,
  meta,
  trackScope = null,
  onNext,
  onBack,
}: PlayerPageProps) {
  const {
    isDesktop,
    snapshot,
    status,
    error,
    sourceUrl,
    isFullscreen,
    open,
    togglePlayback,
    seek,
    setVolume,
    toggleMuted,
    selectTrack,
    toggleFullscreen,
  } = usePlayer();

  const pageRef = useRef<HTMLElement>(null);
  const slotRef = useRef<HTMLDivElement>(null);
  const autoOpenedRef = useRef<string | null>(null);
  const startReportedRef = useRef(false);
  const stopReportedRef = useRef(false);
  const positionRef = useRef(0);
  const lastStatusRef = useRef<PlayerStatus>("idle");
  const overlayActionRef = useRef<(action: OverlayAction) => void>(() => undefined);

  const playbackKey = playback
    ? `${playback.itemId}:${playback.playSessionId ?? ""}`
    : null;

  /** Playback key whose saved track choice has already been applied. */
  const restoredTracksForRef = useRef<string | null>(null);
  const savedTracksRef = useRef<string>("");
  const [nextUnavailable, setNextUnavailable] = useState(false);

  /**
   * Overlay logo. The item's own tag is free (copied by `itemPlaybackMeta`);
   * an episode without one borrows its series' logo through one cached request;
   * no tag at all means no request and the overlay falls back to the title.
   */
  const [logoUrl, setLogoUrl] = useState<string | null>(null);
  const logoTag = meta?.logoTag ?? null;
  const logoSeriesId = meta?.logoSeriesId ?? null;
  const playbackItemId = playback?.itemId ?? null;

  useEffect(() => {
    setLogoUrl(null);
    if (!isDesktop || !playbackItemId) return;
    let cancelled = false;
    const resolve = async (): Promise<string | null> => {
      if (logoTag) return buildLogoUrl(playbackItemId, logoTag);
      if (!logoSeriesId) return null;
      try {
        const tag = await seriesLogoTag(logoSeriesId);
        return tag ? buildLogoUrl(logoSeriesId, tag) : null;
      } catch (reason) {
        console.warn("[player] could not read the series logo", reason);
        return null;
      }
    };
    void resolve().then((url) => {
      if (!cancelled) setLogoUrl(url);
    });
    return () => {
      cancelled = true;
    };
  }, [isDesktop, logoSeriesId, logoTag, playbackItemId]);

  /** What the controls receive: the title row plus the resolved logo URL. */
  const overlayMeta = useMemo(
    () => (meta ? { ...meta, logoUrl } : null),
    [meta, logoUrl],
  );

  const isFullscreenRef = useRef(isFullscreen);
  useEffect(() => {
    isFullscreenRef.current = isFullscreen;
  }, [isFullscreen]);

  useVideoSync(slotRef, isDesktop);

  // The player page owns the borderless window mode. `player_ui_mode` is
  // idempotent, so StrictMode's mount/unmount/mount cycle is safe.
  useEffect(() => {
    if (!isDesktop) return;
    void invokePlayerUiMode("player").catch((reason) => {
      console.warn("[player] could not enter the borderless player mode", reason);
    });
    return () => {
      void invokePlayerUiMode("app").catch((reason) => {
        console.warn("[player] could not restore the app window chrome", reason);
      });
    };
  }, [isDesktop]);

  useEffect(() => {
    startReportedRef.current = false;
    stopReportedRef.current = false;
    lastStatusRef.current = "idle";
  }, [playbackKey]);

  useEffect(() => {
    positionRef.current = snapshot?.positionSeconds ?? 0;
  }, [snapshot?.positionSeconds]);

  useEffect(() => {
    if (!isDesktop || !playback || !playbackKey) return;
    const key = `${playbackKey}:${playback.url}:${playback.startSeconds}`;
    if (autoOpenedRef.current === key) return;
    autoOpenedRef.current = key;
    void open(playback.url, playback.startSeconds);
  }, [isDesktop, open, playback, playbackKey]);

  useEffect(() => {
    if (
      !isDesktop ||
      !playback ||
      startReportedRef.current ||
      !snapshot ||
      (snapshot.status !== "playing" && snapshot.status !== "paused")
    ) {
      return;
    }
    startReportedRef.current = true;
    reportEmbyPlayback(playback, "start", playback.startSeconds, false);
  }, [isDesktop, playback, snapshot]);

  useEffect(() => {
    const previous = lastStatusRef.current;
    lastStatusRef.current = status;
    if (!isDesktop || !playback || !startReportedRef.current) {
      return;
    }
    const position = positionRef.current;
    if (status === "ended" && previous !== "ended") {
      reportEmbyPlayback(playback, "ended", position, false);
    } else if (previous === "paused" && status === "playing") {
      reportEmbyPlayback(playback, "unpause", position, false);
    } else if (
      (previous === "playing" || previous === "loading") &&
      status === "paused"
    ) {
      reportEmbyPlayback(playback, "pause", position, true);
    }
  }, [isDesktop, playback, status]);

  useEffect(() => {
    if (!isDesktop || !playback || status !== "playing") return;
    const timer = window.setInterval(() => {
      reportEmbyPlayback(playback, "progress", positionRef.current, false);
    }, 10_000);
    return () => window.clearInterval(timer);
  }, [isDesktop, playback, status]);

  // Remember the audio/subtitle track per series: restore it once the track
  // list is known, then keep saving whatever the user picks.
  useEffect(() => {
    if (!snapshot || !trackScope || !playbackKey) return;
    if (restoredTracksForRef.current === playbackKey) return;
    if (snapshot.status !== "playing" && snapshot.status !== "paused") return;
    if (snapshot.audioTracks.length === 0 && snapshot.subtitleTracks.length === 0) return;
    restoredTracksForRef.current = playbackKey;
    savedTracksRef.current = "";
    const preference = readTrackPreference(trackScope);
    if (!preference) return;
    const wantedAudio = preference.audio;
    if (wantedAudio) {
      const audio = snapshot.audioTracks.find((track) => sameChoice(track, wantedAudio));
      if (audio && !audio.selected) void selectTrack("audio", audio.id);
    }
    const wantedSubtitle = preference.subtitle;
    if (wantedSubtitle) {
      const subtitle = snapshot.subtitleTracks.find((track) => sameChoice(track, wantedSubtitle));
      if (subtitle && !subtitle.selected) void selectTrack("subtitle", subtitle.id);
    }
  }, [snapshot, trackScope, playbackKey, selectTrack]);

  useEffect(() => {
    if (!snapshot || !trackScope || !playbackKey) return;
    if (restoredTracksForRef.current !== playbackKey) return;
    const audio = snapshot.audioTracks.find((track) => track.selected);
    const subtitle = snapshot.subtitleTracks.find((track) => track.selected);
    if (!audio && !subtitle) return;
    const preference: TrackPreference = {
      ...(audio ? { audio: { lang: audio.lang, title: audio.title } } : {}),
      ...(subtitle ? { subtitle: { lang: subtitle.lang, title: subtitle.title } } : {}),
    };
    const serialized = JSON.stringify(preference);
    if (serialized === savedTracksRef.current) return;
    savedTracksRef.current = serialized;
    writeTrackPreference(trackScope, preference);
  }, [snapshot, trackScope, playbackKey]);

  // mpv statistics exist only while the overlay's info panel is open: the
  // controls window asks for them (overlay-request {stats:true}) and drops the
  // demand when the panel closes, which stops this stream entirely.
  const [statsWanted, setStatsWanted] = useState(false);
  const [stats, setStats] = useState<PlayerStats | null>(null);

  useEffect(() => {
    if (!isDesktop || !playback || !statsWanted) {
      setStats(null);
      return;
    }
    let disposed = false;
    // One request in flight at a time: `player_stats` waits up to 800 ms for
    // mpv, so a 1.5 s timer can otherwise queue overlapping requests.
    let inFlight = false;
    const poll = async () => {
      if (inFlight) return;
      inFlight = true;
      try {
        const next = await invokePlayerStats();
        if (!disposed) setStats(next);
      } catch {
        if (!disposed) setStats(null);
      } finally {
        inFlight = false;
      }
    };
    void poll();
    const timer = window.setInterval(() => void poll(), STATS_POLL_MS);
    return () => {
      disposed = true;
      window.clearInterval(timer);
    };
  }, [isDesktop, playback, statsWanted]);

  // Auto-play the next queued episode shortly after the current one ends.
  useEffect(() => {
    setNextUnavailable(false);
    if (!isDesktop || !playback || !onNext || status !== "ended") return;
    let cancelled = false;
    const timer = window.setTimeout(() => {
      void (async () => {
        const started = await onNext();
        if (!cancelled && !started) setNextUnavailable(true);
      })();
    }, 2000);
    return () => {
      cancelled = true;
      window.clearTimeout(timer);
    };
  }, [isDesktop, onNext, playback, status]);

  useEffect(() => {
    if (!isDesktop) return;
    return () => {
      void invokePlayerStop().catch(() => {
        // The page is already leaving; stop is best-effort teardown.
      });
    };
  }, [isDesktop]);

  useEffect(() => {
    if (!isDesktop || !playback) return;
    return () => {
      if (startReportedRef.current && !stopReportedRef.current) {
        stopReportedRef.current = true;
        reportEmbyPlayback(
          playback,
          "stop",
          positionRef.current,
          lastStatusRef.current === "paused",
        );
      }
    };
  }, [isDesktop, playback]);

  useEffect(() => {
    pageRef.current?.focus({ preventScroll: true });
  }, []);

  useEffect(() => {
    if (isDesktop) return;
    return () => {
      if (document.fullscreenElement) {
        void document.exitFullscreen().catch(() => undefined);
      }
    };
  }, [isDesktop]);

  const leavePlayer = useCallback(() => {
    if (
      isDesktop &&
      playback &&
      startReportedRef.current &&
      !stopReportedRef.current
    ) {
      stopReportedRef.current = true;
      reportEmbyPlayback(
        playback,
        "stop",
        positionRef.current,
        lastStatusRef.current === "paused",
      );
    }
    if (isDesktop && isFullscreenRef.current) {
      void toggleFullscreen();
    }
    onBack();
  }, [isDesktop, onBack, playback, toggleFullscreen]);

  /**
   * The video host is mouse-transparent (`HTTRANSPARENT`), so the click lands on
   * the webview. That does not always activate the Win32 window, which leaves
   * the page focused but the window behind others. Focus it explicitly and wake
   * the separate overlay window so the controls come back with the same click.
   */
  const handleStagePointerDown = useCallback(
    (event: ReactPointerEvent<HTMLElement>) => {
      if (!isDesktop || event.button !== 0) return;
      void getCurrentWindow()
        .setFocus()
        .catch((reason: unknown) => {
          console.warn("[player] could not focus the window", reason);
        });
      void emitTo("controls", "overlay-wake", null).catch((reason: unknown) => {
        console.warn("[player] could not wake the controls overlay", reason);
      });
    },
    [isDesktop],
  );

  const handleSeek = useCallback(
    (seconds: number) => {
      void seek(seconds).then((ok) => {
        if (ok) {
          reportEmbyPlayback(
            playback ?? null,
            "seek",
            seconds,
            lastStatusRef.current === "paused",
          );
        }
      });
    },
    [playback, seek],
  );

  const retryPlayback = useCallback(() => {
    const url = sourceUrl ?? playback?.url;
    if (!url) return;
    const resumeAt = snapshot?.positionSeconds ?? 0;
    void open(url, resumeAt);
  }, [open, playback?.url, snapshot?.positionSeconds, sourceUrl]);

  const handleOverlayAction = useCallback(
    (action: OverlayAction) => {
      switch (action.type) {
        case "toggle-play":
          void togglePlayback();
          break;
        case "seek":
          handleSeek(action.seconds);
          break;
        case "set-volume":
          void setVolume(action.value);
          break;
        case "toggle-mute":
          void toggleMuted();
          break;
        case "set-track":
          void selectTrack(action.kind, action.id === "no" ? -1 : action.id);
          break;
        case "toggle-fullscreen":
          void toggleFullscreen();
          break;
        case "back":
          leavePlayer();
          break;
        case "retry":
          retryPlayback();
          break;
        default:
          break;
      }
    },
    [
      handleSeek,
      leavePlayer,
      retryPlayback,
      selectTrack,
      setVolume,
      toggleFullscreen,
      toggleMuted,
      togglePlayback,
    ],
  );

  useEffect(() => {
    overlayActionRef.current = handleOverlayAction;
  }, [handleOverlayAction]);

  // ---- unified overlay state ------------------------------------------------
  // The controls window is a separate webview that never polls: this page owns
  // the player state and broadcasts the whole surface (snapshot + meta + stats
  // + error) in one message, so what the controls show can never be a mix of
  // two different instants. Position-only changes are coalesced; anything the
  // user can act on (status, error, meta, tracks, volume) goes out at once.
  const errorText = error ?? (snapshot?.status === "error" ? snapshot.error : null);

  /** Never a failure: "ended" and the auto-play hand-off need a status line too. */
  const notice =
    status === "ended"
      ? onNext && !nextUnavailable
        ? "播放结束，正在播放下一集…"
        : "播放结束"
      : null;

  const overlayStateRef = useRef<OverlayStateInput>({
    snapshot: null,
    meta: null,
    stats: null,
    error: null,
    notice: null,
  });
  const lastPublishAtRef = useRef(0);
  const publishTimerRef = useRef<number | null>(null);
  const lastSignatureRef = useRef("");

  const flushOverlayState = useCallback(() => {
    if (publishTimerRef.current !== null) {
      window.clearTimeout(publishTimerRef.current);
      publishTimerRef.current = null;
    }
    lastPublishAtRef.current = Date.now();
    void publishOverlayState(overlayStateRef.current).catch((reason: unknown) => {
      console.warn("[player] could not publish the overlay state", reason);
    });
  }, []);

  /**
   * `urgent` bypasses the throttle. A position tick is not urgent; a play/pause,
   * a track switch or an error is, because the controls window has no other way
   * to learn about it.
   */
  const scheduleOverlayState = useCallback(
    (urgent: boolean) => {
      const now = Date.now();
      const elapsed = now - lastPublishAtRef.current;
      if (urgent || elapsed >= OVERLAY_STATE_THROTTLE_MS) {
        flushOverlayState();
        return;
      }
      if (publishTimerRef.current !== null) return;
      publishTimerRef.current = window.setTimeout(() => {
        publishTimerRef.current = null;
        flushOverlayState();
      }, OVERLAY_STATE_THROTTLE_MS - elapsed);
    },
    [flushOverlayState],
  );

  /** Everything except the position must reach the overlay without delay. */
  const stateSignature = [
    snapshot?.status ?? "idle",
    snapshot?.audioTracks.find((track) => track.selected)?.id ?? "none",
    snapshot?.subtitleTracks.find((track) => track.selected)?.id ?? "none",
    snapshot?.volume ?? 0,
    snapshot?.muted ?? false,
    snapshot?.durationSeconds ?? 0,
    errorText ?? "",
    notice ?? "",
    meta?.title ?? "",
    meta?.subtitle ?? "",
    meta?.chapters?.length ?? 0,
    logoUrl ?? "",
    stats ? 1 : 0,
  ].join("|");

  useEffect(() => {
    if (!isDesktop) return;
    overlayStateRef.current = {
      snapshot: snapshot ?? null,
      // The whole meta, chapters and the resolved logo included — the controls
      // render the chapter menu, the seek markers and the top-left logo from it.
      meta: overlayMeta,
      stats: stats ?? null,
      error: errorText,
      notice,
    };
    const urgent = stateSignature !== lastSignatureRef.current;
    lastSignatureRef.current = stateSignature;
    scheduleOverlayState(urgent);
  }, [errorText, isDesktop, meta, notice, overlayMeta, scheduleOverlayState, snapshot, stateSignature, stats]);

  useEffect(() => {
    return () => {
      if (publishTimerRef.current !== null) {
        window.clearTimeout(publishTimerRef.current);
        publishTimerRef.current = null;
      }
    };
  }, []);

  // A (re)loaded controls window asks once for the current state and states
  // whether it wants the statistics stream; the answer is always a full resend.
  useEffect(() => {
    if (!isDesktop) return;
    let disposed = false;
    let stop: (() => void) | null = null;
    void onOverlayRequest((request) => {
      if (typeof request?.stats === "boolean") setStatsWanted(request.stats);
      scheduleOverlayState(true);
    })
      .then((unlisten) => {
        if (disposed) {
          unlisten();
          return;
        }
        stop = unlisten;
      })
      .catch((reason: unknown) => {
        console.warn("[player] could not listen for overlay requests", reason);
      });
    return () => {
      disposed = true;
      stop?.();
      // Leaving the player clears the controls window: it must not keep the
      // last item's title, chapters or error on screen.
      overlayStateRef.current = {
        snapshot: null,
        meta: null,
        stats: null,
        error: null,
        notice: null,
      };
      void publishOverlayState(overlayStateRef.current).catch(() => {
        // The controls window may already be hidden or gone.
      });
    };
  }, [isDesktop, scheduleOverlayState]);

  // The overlay window is a separate webview: player intents arrive as events
  // so pause/seek/stop reporting stays in this window.
  useEffect(() => {
    if (!isDesktop) return;
    let disposed = false;
    const stops: Array<() => void> = [];
    void listen<OverlayAction>("overlay-action", (event) => {
      overlayActionRef.current(event.payload);
    })
      .then((stop) => {
        if (disposed) {
          stop();
          return;
        }
        stops.push(stop);
      })
      .catch((reason) => {
        console.warn("[player] could not listen for overlay actions", reason);
      });
    // Native stage clicks arrive from the Rust side: the mpv child window eats
    // the real mouse event, so the window implements focus + double-click and
    // forwards the fullscreen intent here.
    void listen("player-toggle-fullscreen", () => {
      overlayActionRef.current({ type: "toggle-fullscreen" });
    })
      .then((stop) => {
        if (disposed) {
          stop();
          return;
        }
        stops.push(stop);
      })
      .catch((reason) => {
        console.warn("[player] could not listen for stage clicks", reason);
      });
    return () => {
      disposed = true;
      for (const stop of stops) stop();
    };
  }, [isDesktop]);

  // Keyboard shortcuts stay on the main window for when the overlay has no
  // focus; the overlay implements the same keys for its own window.
  useEffect(() => {
    const onKeyDown = (event: KeyboardEvent) => {
      if (event.defaultPrevented) return;
      const target = event.target;
      const interactive =
        target instanceof HTMLElement &&
        Boolean(
          target.closest(
            "input, textarea, select, button, a, [contenteditable='true'], [role='menu']",
          ),
        );

      if (event.key === "Escape") {
        event.preventDefault();
        leavePlayer();
        return;
      }

      if (interactive) return;
      if (event.ctrlKey || event.metaKey || event.altKey) return;

      if (event.code === "Space") {
        event.preventDefault();
        void togglePlayback();
        return;
      }

      if (event.key === "ArrowRight" || event.key === "ArrowLeft") {
        if (!snapshot || snapshot.durationSeconds <= 0) return;
        event.preventDefault();
        const delta = event.key === "ArrowRight" ? 10 : -10;
        handleSeek(snapshot.positionSeconds + delta);
        return;
      }

      if (event.key.toLowerCase() === "f") {
        event.preventDefault();
        void toggleFullscreen();
        return;
      }

      if (event.key.toLowerCase() === "m") {
        event.preventDefault();
        void toggleMuted();
      }
    };

    window.addEventListener("keydown", onKeyDown);
    return () => window.removeEventListener("keydown", onKeyDown);
  }, [
    handleSeek,
    leavePlayer,
    snapshot,
    toggleFullscreen,
    toggleMuted,
    togglePlayback,
  ]);

  // One short line at most: the video is the page and covers this text on a
  // real machine — the same strings go to the controls window as `error` and
  // `notice`, which is what the user actually sees.
  const hint = !isDesktop
    ? "需要桌面环境"
    : errorText ??
      notice ??
      (status === "error"
        ? "播放失败"
        : status === "idle"
          ? "等待播放"
          : status === "loading"
            ? "正在载入"
            : null);

  return (
    <main
      className="player-layout"
      ref={pageRef}
      tabIndex={-1}
      aria-label="播放器"
    >
      <section
        className="player-stage"
        aria-label="播放器画面"
        onPointerDown={handleStagePointerDown}
      >
        <div
          className="player-video-slot"
          ref={slotRef}
          role="region"
          aria-label="视频画面区域，双击切换全屏"
          onDoubleClick={isDesktop ? undefined : () => void toggleFullscreen()}
        >
          {hint && (
            <p
              className={`player-hint${errorText ? " player-hint--error" : ""}`}
              role={errorText ? "alert" : undefined}
            >
              {hint}
            </p>
          )}
        </div>
      </section>
      <span className="visually-hidden" aria-live="polite">
        {statusLabel(status)}
        {snapshot?.buffering && status !== "loading" ? " · 缓冲中" : ""}
      </span>
    </main>
  );
}