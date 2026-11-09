import { useCallback, useEffect, useRef } from "react";
import type { PointerEvent as ReactPointerEvent } from "react";
import { emitTo, listen } from "@tauri-apps/api/event";
import { getCurrentWindow } from "@tauri-apps/api/window";
import { invokePlayerUiMode } from "../controls/bridge";
import type { OverlayAction } from "../controls/contract";
import type { EmbyPlaybackTarget } from "../emby/contract";
import { reportEmbyPlayback } from "../emby/playback";
import type { PlayerStatus } from "./contract";
import { invokePlayerStop } from "./tauri";
import { usePlayer } from "./usePlayer";
import { useVideoSync } from "./useVideoSync";

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
  onBack: () => void;
}

/**
 * The playback page is intentionally chrome-free: the native mpv window covers
 * the webview, so every control lives in the transparent `controls.html`
 * overlay window and talks back through `overlay-action`.
 */
export default function PlayerPage({ playback, onBack }: PlayerPageProps) {
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

  // The overlay window is a separate webview: player intents arrive as events
  // so pause/seek/stop reporting stays in this window.
  useEffect(() => {
    if (!isDesktop) return;
    let disposed = false;
    let unlisten: (() => void) | null = null;
    void listen<OverlayAction>("overlay-action", (event) => {
      overlayActionRef.current(event.payload);
    })
      .then((stop) => {
        if (disposed) {
          stop();
          return;
        }
        unlisten = stop;
      })
      .catch((reason) => {
        console.warn("[player] could not listen for overlay actions", reason);
      });
    return () => {
      disposed = true;
      unlisten?.();
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

  const errorText =
    error ?? (snapshot?.status === "error" ? snapshot.error : null);

  // One short line at most: the video is the page.
  const hint = !isDesktop
    ? "需要桌面环境"
    : errorText ??
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
          onDoubleClick={() => void toggleFullscreen()}
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
