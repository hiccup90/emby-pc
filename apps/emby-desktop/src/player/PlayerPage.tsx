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
import type { PlaybackMeta, PlayerStats, PlayerStatus, SkipIntroMode } from "./contract";
import { invokePlayerStats, invokePlayerStop } from "./tauri";
import {
  findTrackIndexByChoice,
  readTrackPreference,
  writeTrackPreference,
} from "./trackPreference";
import type { TrackPreference } from "./trackPreference";
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
  /** Start the next episode; resolves false when there is none. */
  onNext?: () => Promise<boolean>;
  /** Start the previous episode; resolves false when there is none. */
  onPrevious?: () => Promise<boolean>;
  /**
   * The shell's last launch failure, i.e. it could not obtain a playable URL
   * from the server (PlaybackInfo). Episode stepping bypasses the browsing
   * shell, so the banner that normally shows this is not on screen: without
   * this prop a failed "next episode" would look like a dead button.
   */
  launchError?: string | null;
  /** Whether finishing an episode auto-advances (the "autoplay" setting). */
  autoPlayNext?: boolean;
  /** Emby intro markers: off, show the button, or skip automatically. */
  skipIntro?: SkipIntroMode;
  /**
   * Ordered ids of the current series' episodes, from the shell's queue. Null
   * means "not an episode": the controls then show neither transport button.
   * The player works out the neighbours from this list, and the overlay never
   * queries the server.
   */
  episodeRun?: readonly string[] | null;
  onBack: () => void;
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
  onPrevious,
  launchError = null,
  autoPlayNext = false,
  skipIntro = "auto",
  episodeRun = null,
  onBack,
}: PlayerPageProps) {
  const {
    isDesktop,
    snapshot,
    status,
    error,
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
  /** Latest mute state, for reports sent outside a render (stop/unmount). */
  const mutedRef = useRef(false);
  const lastStatusRef = useRef<PlayerStatus>("idle");
  const overlayActionRef = useRef<(action: OverlayAction) => void>(() => undefined);

  const playbackKey = playback
    ? `${playback.itemId}:${playback.playSessionId ?? ""}`
    : null;
  /** Intro key already auto-skipped for the current episode. */
  const autoSkippedRef = useRef<string | null>(null);
  /** Playback key whose `player_open` call has completed successfully. */
  const openedPlaybackRef = useRef<string | null>(null);

  /** Playback key whose saved track choice has already been applied. */
  const restoredTracksForRef = useRef<string | null>(null);
  const savedTracksRef = useRef<string>("");
  /**
   * Whether the current selection came from the user. A restore must not be
   * written back, and the freshly loaded file's default tracks must not
   * overwrite the saved preference either: only an explicit pick does.
   */
  const userPickedTracksRef = useRef(false);
  const [nextUnavailable, setNextUnavailable] = useState(false);
  /**
   * Label of the last failed previous/next press, or null. The transport
   * buttons live in the controls window, so a rejected step would otherwise
   * have nowhere to report itself: `void onNext()` used to drop the result.
   */
  const [stepFailure, setStepFailure] = useState<string | null>(null);

  /**
   * Overlay logo. The item's own tag is free (copied by `itemPlaybackMeta`);
   * an episode without one borrows its series' logo through one cached request;
   * no tag at all means no request and the overlay falls back to the title.
   */
  const [logoUrl, setLogoUrl] = useState<string | null>(null);
  const logoTag = meta?.logoTag ?? null;
  const logoSeriesId = meta?.logoSeriesId ?? null;
  const playbackItemId = playback?.itemId ?? null;

  /**
   * Episode navigation, worked out from the shell's ordered episode run: the
   * position of the item on screen decides both neighbours. Null for anything
   * that is not an episode, which is what removes the buttons entirely.
   */
  const episodeIndex = episodeRun && playbackItemId ? episodeRun.indexOf(playbackItemId) : -1;
  const episodeNav = useMemo(
    () =>
      episodeRun && episodeIndex >= 0
        ? { hasPrev: episodeIndex > 0, hasNext: episodeIndex + 1 < episodeRun.length }
        : null,
    [episodeIndex, episodeRun],
  );

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

  /**
   * What the controls receive: the title row, the resolved logo URL and the
   * episode neighbours. Only episodes get neighbours, so a movie leaves both
   * flags undefined and the controls render neither transport button.
   */
  const overlayMeta = useMemo(
    () =>
      meta
        ? {
            ...meta,
            logoUrl,
            // Only episodes carry neighbours; a movie has neither key and the
            // controls then render no episode transport at all.
            ...(episodeNav
              ? { hasPrevEpisode: episodeNav.hasPrev, hasNextEpisode: episodeNav.hasNext }
              : {}),
            // Auto mode still carries `intro` for the seek, but the controls
            // must not show a manual button that the automatic skip already
            // handles. Off mode carries neither.
            skipIntroButton: skipIntro === "button",
          }
        : null,
    [episodeNav, meta, logoUrl, skipIntro],
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
    // A new item on screen supersedes whatever the last step press reported.
    setStepFailure(null);
    // A new episode gets its own auto-skip budget.
    autoSkippedRef.current = null;
    openedPlaybackRef.current = null;
    userPickedTracksRef.current = false;
  }, [playbackKey]);

  useEffect(() => {
    positionRef.current = snapshot?.positionSeconds ?? 0;
    mutedRef.current = snapshot?.muted ?? false;
  }, [snapshot?.positionSeconds, snapshot?.muted]);

  useEffect(() => {
    if (!isDesktop || !playback || !playbackKey) return;
    const key = `${playbackKey}:${playback.url}:${playback.startSeconds}`;
    if (autoOpenedRef.current === key) return;
    autoOpenedRef.current = key;
    openedPlaybackRef.current = null;
    const openedKey = playbackKey;
    void open(playback.url, playback.startSeconds).then((opened) => {
      if (opened && autoOpenedRef.current === key) {
        openedPlaybackRef.current = openedKey;
      }
    });
  }, [isDesktop, open, playback, playbackKey]);

  useEffect(() => {
    if (
      !isDesktop ||
      !playback ||
      startReportedRef.current ||
      !snapshot ||
      // `playbackKey` changes before `player_open` returns, and until it does
      // the snapshot still belongs to the previous file: reporting `start` for
      // the new item off that snapshot fires while the old frame is on screen
      // (observed as a 12 ms gap after PlaybackInfo) and skews Emby's play
      // history and the intro plugin's START event.
      openedPlaybackRef.current !== playbackKey ||
      (snapshot.status !== "playing" && snapshot.status !== "paused")
    ) {
      return;
    }
    startReportedRef.current = true;
    reportEmbyPlayback(playback, "start", playback.startSeconds, false, mutedRef.current);
  }, [isDesktop, playback, snapshot]);

  useEffect(() => {
    const previous = lastStatusRef.current;
    lastStatusRef.current = status;
    if (!isDesktop || !playback || !startReportedRef.current) {
      return;
    }
    const position = positionRef.current;
    if (status === "ended" && previous !== "ended") {
      reportEmbyPlayback(playback, "ended", position, false, mutedRef.current);
    } else if (previous === "paused" && status === "playing") {
      reportEmbyPlayback(playback, "unpause", position, false, mutedRef.current);
    } else if (
      (previous === "playing" || previous === "loading") &&
      status === "paused"
    ) {
      reportEmbyPlayback(playback, "pause", position, true, mutedRef.current);
    }
  }, [isDesktop, playback, status]);

  useEffect(() => {
    if (!isDesktop || !playback || status !== "playing") return;
    const timer = window.setInterval(() => {
      reportEmbyPlayback(playback, "progress", positionRef.current, false, mutedRef.current);
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
      const index = findTrackIndexByChoice(snapshot.audioTracks, wantedAudio);
      const audio = index >= 0 ? snapshot.audioTracks[index] : null;
      if (audio && !audio.selected) void selectTrack("audio", audio.id);
    }
    if (preference.subtitle === null) {
      if (snapshot.subtitleTracks.some((track) => track.selected)) {
        void selectTrack("subtitle", -1);
      }
    } else if (preference.subtitle) {
      const index = findTrackIndexByChoice(snapshot.subtitleTracks, preference.subtitle);
      const subtitle = index >= 0 ? snapshot.subtitleTracks[index] : null;
      if (subtitle && !subtitle.selected) void selectTrack("subtitle", subtitle.id);
    }
  }, [snapshot, trackScope, playbackKey, selectTrack]);

  useEffect(() => {
    if (!snapshot || !trackScope || !playbackKey) return;
    if (restoredTracksForRef.current !== playbackKey) return;
    if (!userPickedTracksRef.current) return;
    const audio = snapshot.audioTracks.find((track) => track.selected);
    const subtitle = snapshot.subtitleTracks.find((track) => track.selected);
    if (!audio && snapshot.subtitleTracks.length === 0) return;
    const preference: TrackPreference = {
      ...(audio ? { audio: { lang: audio.lang, title: audio.title } } : {}),
      // There are subtitles but none selected: remember "off" explicitly so
      // the next launch does not turn the default subtitle back on.
      ...(snapshot.subtitleTracks.length > 0
        ? { subtitle: subtitle ? { lang: subtitle.lang, title: subtitle.title } : null }
        : {}),
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
    if (!isDesktop || !playback || !onNext || !autoPlayNext || status !== "ended") return;
    // Only episodes with a known next neighbour auto-advance: the last episode
    // of a run (and anything that is not an episode) has nowhere to go.
    if (episodeNav?.hasNext !== true) return;
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
  }, [autoPlayNext, episodeNav, isDesktop, onNext, playback, status]);

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
          mutedRef.current,
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
    // The unmount effect below reports `stop`; doing it here as well duplicated
    // the same report (and its position) before the route changed.
    if (isDesktop && isFullscreenRef.current) {
      void toggleFullscreen();
    }
    onBack();
  }, [isDesktop, onBack, toggleFullscreen]);

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
            mutedRef.current,
          );
        }
      });
    },
    [playback, seek],
  );

  /**
   * Auto mode: seek to IntroEnd once per segment. The "skipped" key is cleared
   * only after the playhead leaves the segment on the left (a manual rewind
   * before IntroStart), so a normal seek or a late snapshot cannot loop.
   */
  const intro = meta?.intro ?? null;
  const autoSkipKey =
    intro && playbackKey
      ? `${playbackKey}:${intro.startSeconds}:${intro.endSeconds}`
      : null;
  useEffect(() => {
    if (skipIntro !== "auto" || !intro || !autoSkipKey || !playbackKey) return;
    // A route change leaves the previous episode's snapshot in place until
    // `player_open` resolves; seeking it would hit the old file.
    if (openedPlaybackRef.current !== playbackKey) return;
    if (!snapshot || snapshot.status !== "playing" || snapshot.durationSeconds <= 0) return;
    const position = snapshot.positionSeconds;
    if (position < intro.startSeconds) {
      autoSkippedRef.current = null;
      return;
    }
    if (position >= intro.endSeconds) return;
    if (autoSkippedRef.current === autoSkipKey) return;
    autoSkippedRef.current = autoSkipKey;
    handleSeek(Math.min(intro.endSeconds, snapshot.durationSeconds));
  }, [
    autoSkipKey,
    handleSeek,
    intro,
    playbackKey,
    skipIntro,
    snapshot?.durationSeconds,
    snapshot?.positionSeconds,
    snapshot?.seq,
    snapshot?.status,
  ]);

  const retryPlayback = useCallback(() => {
    if (!playback || !playbackKey) return;
    openedPlaybackRef.current = null;
    void open(playback.url, playback.startSeconds).then((opened) => {
      if (opened) openedPlaybackRef.current = playbackKey;
    });
  }, [open, playback, playbackKey]);

  /**
   * Run one previous/next press and keep its outcome.
   *
   * `onNext`/`onPrevious` resolve `false` when the shell could not start the
   * neighbouring episode (no run, end of the run, or a failed PlaybackInfo).
   * That failure is invisible from the controls window, which only ever sees
   * the broadcast state, so it is turned into the page's own `stepFailure`
   * line here. The shell's detailed `launchError` arrives one render later and
   * `errorText` prefers it, so the user ends up with the real reason.
   */
  const runEpisodeStep = useCallback(
    async (direction: "previous" | "next") => {
      const action = direction === "next" ? onNext : onPrevious;
      if (!action) return;
      const label = direction === "next" ? "切换到下一集失败" : "切换到上一集失败";
      setStepFailure(null);
      try {
        const started = await action();
        if (!started) setStepFailure(label);
      } catch (reason) {
        console.warn(`[player] ${direction} episode failed`, reason);
        setStepFailure(label);
      }
    },
    [onNext, onPrevious],
  );

  const handleOverlayAction = useCallback(
    (action: OverlayAction) => {
      switch (action.type) {
        case "toggle-play":
          void togglePlayback(playback?.url);
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
          userPickedTracksRef.current = true;
          void selectTrack(action.kind, action.id === "no" ? -1 : action.id);
          break;
        case "toggle-fullscreen":
          void toggleFullscreen();
          break;
        case "back":
          leavePlayer();
          break;
        case "previous":
          void runEpisodeStep("previous");
          break;
        case "next":
          void runEpisodeStep("next");
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
      runEpisodeStep,
      selectTrack,
      setVolume,
      toggleFullscreen,
      toggleMuted,
      togglePlayback,
      playback?.url,
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
  // The three ways playback can fail have three distinct sources, so the label
  // below needs no string matching:
  //   launchError        - the shell could not get a playable URL (PlaybackInfo)
  //   error              - `player_open` rejected it (no stream confirmation in
  //                        time, or the mpv worker was gone)
  //   snapshot.error     - mpv took the file and failed later (END_FILE/error)
  // Distinguishing them is what makes a single user report point at one stage.
  const stageError =
    launchError !== null
      ? `取流失败：${launchError}`
      : error !== null
        ? `播放器启动失败：${error}`
        : snapshot?.status === "error" && snapshot.error
          ? `播放中断：${snapshot.error}`
          : null;
  const errorText = stepFailure
    ? stageError
      ? `${stepFailure}（${stageError}）`
      : `${stepFailure}：没有可播放的剧集，或取流失败`
    : stageError;

  /** Never a failure: "ended" and the auto-play hand-off need a status line too. */
  const notice =
    status === "ended"
      ? autoPlayNext && onNext && !nextUnavailable && episodeNav?.hasNext === true
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
    meta?.intro?.startSeconds ?? -1,
    meta?.intro?.endSeconds ?? -1,
    skipIntro === "button" ? 1 : 0,
    logoUrl ?? "",
    episodeNav ? `${episodeNav.hasPrev}:${episodeNav.hasNext}` : "none",
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
  }, [
    episodeNav,
    errorText,
    isDesktop,
    meta,
    notice,
    overlayMeta,
    scheduleOverlayState,
    snapshot,
    stateSignature,
    stats,
  ]);

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
  // focus; the overlay implements the same keys for its own window. The
  // listener is registered once and reads current state/actions through refs;
  // depending on `snapshot` rebuilt it several times per second.
  const snapshotRef = useRef(snapshot);
  snapshotRef.current = snapshot;
  const playbackRef = useRef(playback);
  playbackRef.current = playback;
  const keyActionsRef = useRef({
    leavePlayer,
    handleSeek,
    togglePlayback,
    toggleFullscreen,
    toggleMuted,
  });
  keyActionsRef.current = {
    leavePlayer,
    handleSeek,
    togglePlayback,
    toggleFullscreen,
    toggleMuted,
  };

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
        keyActionsRef.current.leavePlayer();
        return;
      }

      if (interactive) return;
      if (event.ctrlKey || event.metaKey || event.altKey) return;

      if (event.code === "Space") {
        event.preventDefault();
        void keyActionsRef.current.togglePlayback(playbackRef.current?.url);
        return;
      }

      const current = snapshotRef.current;
      if (event.key === "ArrowRight" || event.key === "ArrowLeft") {
        if (!current || current.durationSeconds <= 0) return;
        event.preventDefault();
        const delta = event.key === "ArrowRight" ? 10 : -10;
        keyActionsRef.current.handleSeek(current.positionSeconds + delta);
        return;
      }

      if (event.key.toLowerCase() === "f") {
        event.preventDefault();
        void keyActionsRef.current.toggleFullscreen();
        return;
      }

      if (event.key.toLowerCase() === "m") {
        event.preventDefault();
        void keyActionsRef.current.toggleMuted();
      }
    };

    window.addEventListener("keydown", onKeyDown);
    return () => window.removeEventListener("keydown", onKeyDown);
  }, []);

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