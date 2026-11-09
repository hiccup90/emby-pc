import { useCallback, useEffect, useRef, useState } from "react";
import type { FormEvent, ReactNode, RefObject } from "react";
import type { EmbyPlaybackTarget } from "../emby/contract";
import { compactUrl } from "../emby/format";
import { reportEmbyPlayback } from "../emby/playback";
import type { PlayerStatus, PlayerTrack } from "./contract";
import { invokePlayerStop } from "./tauri";
import { usePlayer } from "./usePlayer";
import type { PlayerTrackKind } from "./usePlayer";
import { useVideoSync } from "./useVideoSync";

type PlayerIconName =
  | "arrow-left"
  | "play"
  | "pause"
  | "volume"
  | "volume-off"
  | "captions"
  | "audio"
  | "chevron"
  | "fullscreen";

function PlayerIcon({ name, size = 18 }: { name: PlayerIconName; size?: number }) {
  const shared = {
    fill: "none",
    stroke: "currentColor",
    strokeWidth: 1.7,
    strokeLinecap: "round" as const,
    strokeLinejoin: "round" as const,
  };
  const paths: Record<PlayerIconName, ReactNode> = {
    "arrow-left": (
      <>
        <path d="m11 18-6-6 6-6" />
        <path d="M5 12h14" />
      </>
    ),
    play: <path d="m8 5 11 7-11 7z" fill="currentColor" stroke="none" />,
    pause: (
      <>
        <path d="M8 5h3v14H8z" fill="currentColor" stroke="none" />
        <path d="M15 5h3v14h-3z" fill="currentColor" stroke="none" />
      </>
    ),
    volume: (
      <>
        <path d="M4 10v4h4l5 4V6l-5 4H4z" />
        <path d="M16 9a5 5 0 0 1 0 6" />
        <path d="M18 6a9 9 0 0 1 0 12" />
      </>
    ),
    "volume-off": (
      <>
        <path d="M4 10v4h4l5 4V6l-5 4H4z" />
        <path d="m17 9 5 6m0-6-5 6" />
      </>
    ),
    captions: (
      <>
        <rect x="3" y="5" width="18" height="14" rx="2" />
        <path d="M8 10H7a2 2 0 0 0 0 4h1m9-4h-1a2 2 0 0 0 0 4h1" />
      </>
    ),
    audio: (
      <>
        <path d="M4 10v4h4l5 4V6l-5 4H4z" />
        <path d="M17 9a5 5 0 0 1 0 6" />
      </>
    ),
    chevron: <path d="m7 10 5 5 5-5" />,
    fullscreen: <path d="M8 4H4v4m12-4h4v4M8 20H4v-4m12 4h4v-4" />,
  };

  return (
    <svg aria-hidden="true" width={size} height={size} viewBox="0 0 24 24" {...shared}>
      {paths[name]}
    </svg>
  );
}

function formatTime(seconds: number): string {
  const safe = Math.max(0, Math.floor(seconds));
  const hours = Math.floor(safe / 3600);
  const minutes = Math.floor((safe % 3600) / 60);
  const remaining = safe % 60;
  return hours > 0
    ? `${hours}:${String(minutes).padStart(2, "0")}:${String(remaining).padStart(2, "0")}`
    : `${minutes}:${String(remaining).padStart(2, "0")}`;
}

function statusLabel(status: PlayerStatus, preview: boolean): string {
  const labels: Record<PlayerStatus, string> = {
    idle: "尚未载入媒体",
    loading: "正在载入",
    playing: "播放中",
    paused: "已暂停",
    ended: "播放结束",
    error: "播放错误",
  };
  const label = labels[status];
  return preview && status !== "idle" && status !== "error" ? `预览 · ${label}` : label;
}

interface TrackControlProps {
  kind: PlayerTrackKind;
  label: string;
  icon: "audio" | "captions";
  tracks: PlayerTrack[];
  open: boolean;
  disabled: boolean;
  triggerRef: RefObject<HTMLButtonElement>;
  onToggle: () => void;
}

function TrackControl({
  kind,
  label,
  icon,
  tracks,
  open,
  disabled,
  triggerRef,
  onToggle,
}: TrackControlProps) {
  const panelId = `player-track-panel-${kind}`;
  const currentTitle =
    tracks.find((track) => track.selected)?.title ?? (kind === "subtitle" ? "关闭" : "默认");

  return (
    <div className="track-control">
      <button
        ref={triggerRef}
        type="button"
        className={`tool-button${open ? " is-selected" : ""}`}
        aria-label={`${label}：${currentTitle}`}
        aria-expanded={open}
        aria-haspopup="menu"
        aria-controls={open ? panelId : undefined}
        disabled={disabled}
        onClick={onToggle}
      >
        <PlayerIcon name={icon} size={17} />
        <span className="visually-hidden">{label}</span>
      </button>
    </div>
  );
}

interface TrackPanelProps {
  kind: PlayerTrackKind;
  label: string;
  tracks: PlayerTrack[];
  onSelect: (id: number) => void;
  onClose: () => void;
}

function TrackPanel({ kind, label, tracks, onSelect, onClose }: TrackPanelProps) {
  const panelId = `player-track-panel-${kind}`;
  const options: PlayerTrack[] =
    kind === "subtitle"
      ? [
          ...tracks.filter((track) => track.id !== -1),
          {
            id: -1,
            type: "subtitle",
            title: "关闭字幕",
            lang: null,
            selected: !tracks.some((track) => track.selected),
          },
        ]
      : tracks;

  return (
    <section className="player-track-panel" id={panelId} aria-label={`${label}选择`}>
      <div className="player-track-panel__head">
        <span className="player-track-panel__label">{label}</span>
        <button
          className="player-track-panel__close"
          type="button"
          onClick={onClose}
          aria-label={`关闭${label}菜单`}
        >
          关闭
        </button>
      </div>
      <div className="player-track-panel__options" role="menu" aria-label={`选择${label}`}>
        {options.length === 0 ? (
          <span className="player-track-panel__empty">暂无可用轨道</span>
        ) : (
          options.map((track) => (
            <button
              key={`${kind}-${track.id}`}
              type="button"
              role="menuitemradio"
              aria-checked={track.selected}
              className={`player-track-panel__option${track.selected ? " is-current" : ""}`}
              onClick={() => onSelect(track.id)}
            >
              <span className="player-track-panel__title">
                {track.title}
                {track.lang && <small>{track.lang}</small>}
              </span>
              {track.selected && (
                <span className="player-track-panel__check" aria-hidden="true">
                  ✓
                </span>
              )}
            </button>
          ))
        )}
      </div>
    </section>
  );
}

interface PlayerPageProps {
  title: string;
  playback?: EmbyPlaybackTarget;
  onBack: () => void;
}

export default function PlayerPage({ title, playback, onBack }: PlayerPageProps) {
  const {
    isDesktop,
    snapshot,
    status,
    error,
    isOpening,
    sourceUrl,
    isFullscreen,
    open,
    togglePlayback,
    seek,
    setVolume,
    toggleMuted,
    selectTrack,
    toggleFullscreen,
    dismissError,
  } = usePlayer();

  const pageRef = useRef<HTMLElement>(null);
  const slotRef = useRef<HTMLDivElement>(null);
  const controlsRef = useRef<HTMLElement>(null);
  const urlInputRef = useRef<HTMLInputElement>(null);
  const seekInputRef = useRef<HTMLInputElement>(null);
  const playButtonRef = useRef<HTMLButtonElement>(null);
  const showControlsButtonRef = useRef<HTMLButtonElement>(null);
  const audioTriggerRef = useRef<HTMLButtonElement>(null);
  const subtitleTriggerRef = useRef<HTMLButtonElement>(null);
  const hideTimerRef = useRef<number | null>(null);
  const trackMenuRef = useRef<PlayerTrackKind | null>(null);
  const pendingFocusRef = useRef<"show" | "play" | null>(null);
  const seekPreviewRef = useRef<number | null>(null);
  const autoOpenedRef = useRef<string | null>(null);
  const manualModeRef = useRef(false);
  const startReportedRef = useRef(false);
  const stopReportedRef = useRef(false);
  const positionRef = useRef(0);
  const lastStatusRef = useRef<PlayerStatus>("idle");

  const playbackKey = playback
    ? `${playback.itemId}:${playback.playSessionId ?? ""}`
    : null;

  const [url, setUrl] = useState("");
  const [controlsVisible, setControlsVisible] = useState(true);
  const [trackMenu, setTrackMenu] = useState<PlayerTrackKind | null>(null);
  const [seekPreview, setSeekPreview] = useState<number | null>(null);

  useVideoSync(slotRef, isDesktop);

  useEffect(() => {
    startReportedRef.current = false;
    stopReportedRef.current = false;
    manualModeRef.current = false;
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
    manualModeRef.current = false;
    void open(playback.url, playback.startSeconds);
  }, [isDesktop, open, playback, playbackKey]);

  useEffect(() => {
    if (
      !isDesktop ||
      !playback ||
      manualModeRef.current ||
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
    if (!isDesktop || !playback || manualModeRef.current || !startReportedRef.current) {
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
      if (manualModeRef.current) return;
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
      if (
        startReportedRef.current &&
        !stopReportedRef.current &&
        !manualModeRef.current
      ) {
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

  useEffect(() => {
    trackMenuRef.current = trackMenu;
  }, [trackMenu]);

  const clearHideTimer = useCallback(() => {
    if (hideTimerRef.current !== null) {
      window.clearTimeout(hideTimerRef.current);
      hideTimerRef.current = null;
    }
  }, []);

  const armHideTimer = useCallback(() => {
    clearHideTimer();
    hideTimerRef.current = window.setTimeout(() => {
      hideTimerRef.current = null;
      if (trackMenuRef.current !== null) return;
      const active = document.activeElement;
      if (active instanceof HTMLElement && controlsRef.current?.contains(active)) return;
      setControlsVisible(false);
    }, 2500);
  }, [clearHideTimer]);

  const wakeControls = useCallback(() => {
    setControlsVisible(true);
    armHideTimer();
  }, [armHideTimer]);

  const hideControls = useCallback(() => {
    clearHideTimer();
    setTrackMenu(null);
    pendingFocusRef.current = "show";
    setControlsVisible(false);
  }, [clearHideTimer]);

  const showControls = useCallback(() => {
    pendingFocusRef.current = "play";
    setControlsVisible(true);
  }, []);

  useEffect(() => {
    if (
      !controlsVisible ||
      trackMenu !== null ||
      error !== null ||
      isOpening ||
      snapshot?.status === "error"
    ) {
      clearHideTimer();
      return;
    }
    armHideTimer();
    return clearHideTimer;
  }, [
    armHideTimer,
    clearHideTimer,
    controlsVisible,
    error,
    isOpening,
    snapshot?.status,
    trackMenu,
  ]);

  useEffect(() => {
    const pending = pendingFocusRef.current;
    if (pending === null) return;
    pendingFocusRef.current = null;
    const target =
      pending === "show"
        ? showControlsButtonRef.current
        : playButtonRef.current && !playButtonRef.current.disabled
          ? playButtonRef.current
          : urlInputRef.current;
    target?.focus({ preventScroll: true });
  }, [controlsVisible]);

  useEffect(() => {
    if (!error && !(snapshot?.status === "error" && snapshot.error)) return;
    setControlsVisible(true);
    setTrackMenu(null);
  }, [error, snapshot?.error, snapshot?.status]);

  useEffect(() => {
    if (trackMenu === null) return;
    const onPointerDown = (event: PointerEvent) => {
      const target = event.target;
      if (
        target instanceof Element &&
        (target.closest(".track-control") || target.closest(".player-track-panel"))
      ) {
        return;
      }
      setTrackMenu(null);
    };
    document.addEventListener("pointerdown", onPointerDown);
    return () => document.removeEventListener("pointerdown", onPointerDown);
  }, [trackMenu]);

  const updateSeekPreview = useCallback((value: number | null) => {
    seekPreviewRef.current = value;
    setSeekPreview(value);
  }, []);

  const commitSeek = useCallback(
    (value: number) => {
      if (seekPreviewRef.current === null) return;
      if (!Number.isFinite(value)) return;
      updateSeekPreview(null);
      void seek(value).then((ok) => {
        if (ok && !manualModeRef.current) {
          reportEmbyPlayback(
            playback ?? null,
            "seek",
            value,
            status === "paused",
          );
        }
      });
    },
    [playback, seek, status, updateSeekPreview],
  );

  const closeTrackMenu = useCallback(() => {
    const trigger =
      trackMenu === "audio"
        ? audioTriggerRef.current
        : trackMenu === "subtitle"
          ? subtitleTriggerRef.current
          : null;
    setTrackMenu(null);
    trigger?.focus({ preventScroll: true });
  }, [trackMenu]);

  const leavePlayer = useCallback(() => {
    if (
      isDesktop &&
      playback &&
      !manualModeRef.current &&
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
    if (isDesktop && isFullscreen) {
      void toggleFullscreen();
    }
    onBack();
  }, [isDesktop, isFullscreen, onBack, playback, toggleFullscreen]);

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
        if (trackMenu !== null) {
          event.preventDefault();
          closeTrackMenu();
          return;
        }
        event.preventDefault();
        leavePlayer();
        return;
      }

      wakeControls();
      if (interactive) return;

      if (event.code === "Space") {
        event.preventDefault();
        void togglePlayback();
        return;
      }

      if (event.key === "ArrowRight" || event.key === "ArrowLeft") {
        if (!snapshot || snapshot.durationSeconds <= 0) return;
        event.preventDefault();
        const delta = event.key === "ArrowRight" ? 10 : -10;
        void seek(snapshot.positionSeconds + delta);
        return;
      }

      if (
        event.key.toLowerCase() === "f" &&
        !event.ctrlKey &&
        !event.metaKey &&
        !event.altKey
      ) {
        event.preventDefault();
        void toggleFullscreen();
      }
    };

    window.addEventListener("keydown", onKeyDown);
    return () => window.removeEventListener("keydown", onKeyDown);
  }, [
    closeTrackMenu,
    leavePlayer,
    seek,
    snapshot,
    toggleFullscreen,
    togglePlayback,
    trackMenu,
    wakeControls,
  ]);

  const handleOpenSubmit = (event: FormEvent<HTMLFormElement>) => {
    event.preventDefault();
    manualModeRef.current = true;
    updateSeekPreview(null);
    setTrackMenu(null);
    wakeControls();
    void open(url);
  };

  const toggleTrackMenu = (kind: PlayerTrackKind) => {
    wakeControls();
    setTrackMenu((current) => (current === kind ? null : kind));
  };

  const handleSelectTrack = (kind: PlayerTrackKind, id: number) => {
    setTrackMenu(null);
    const trigger = kind === "audio" ? audioTriggerRef.current : subtitleTriggerRef.current;
    trigger?.focus({ preventScroll: true });
    void selectTrack(kind, id);
  };

  const duration = snapshot?.durationSeconds ?? 0;
  const position = seekPreview ?? snapshot?.positionSeconds ?? 0;
  const boundedPosition = duration > 0 ? Math.min(Math.max(0, position), duration) : 0;
  const progressPercent = duration > 0 ? (boundedPosition / duration) * 100 : 0;
  const volume = snapshot?.volume ?? 72;
  const muted = snapshot?.muted ?? false;
  const hasMedia = Boolean(snapshot && status !== "idle" && status !== "error");
  const canSeek = Boolean(snapshot && duration > 0 && status !== "idle" && status !== "error");
  const isPlaying = status === "playing" || status === "loading";
  const snapshotError = snapshot?.status === "error" ? snapshot.error : null;
  const visibleError = error ?? snapshotError;
  const label = statusLabel(status, !isDesktop);

  return (
    <main
      className="player-layout"
      ref={pageRef}
      tabIndex={-1}
      onPointerMove={wakeControls}
      aria-label="播放器"
    >
      <header className="player-topbar">
        <button
          className="icon-button"
          type="button"
          onClick={leavePlayer}
          aria-label="返回详情"
          title="返回详情"
        >
          <PlayerIcon name="arrow-left" size={19} />
        </button>
        <div className="player-title" title={title}>
          {title}
        </div>
      </header>

      {!isDesktop && (
        <div className="player-preview-note" role="status">
          需要 Tauri 桌面环境才能驱动原生 mpv 窗口；当前为浏览器预览，播放命令不会发送，控件仅更新本地状态。
        </div>
      )}

      <section className="player-stage" aria-label="播放器画面">
        <div
          className="player-video-slot"
          ref={slotRef}
          role="region"
          aria-label="视频画面区域，双击切换全屏"
          onDoubleClick={() => void toggleFullscreen()}
        >
          <span className="player-video-slot__badge" aria-hidden="true">
            <span className={`status-square status-square--${status}`} />
            {label}
            {snapshot?.buffering && status !== "loading" ? " · 缓冲中" : ""}
          </span>

          {!hasMedia && (
            <div className="player-empty">
              <h1 className="player-empty__title">
                {status === "error" ? "播放失败" : title}
              </h1>
              <p className="player-empty__text">
                {status === "error"
                  ? snapshotError ?? "请重新输入播放地址后重试。"
                  : playback
                    ? "正在通过 Emby 获取播放直链，随后交给本机 mpv。"
                    : "展开“调试直链”并输入地址，验证前端控制命令与原生 mpv 的链路。"}
              </p>
              <p className="player-empty__hint">
                {isDesktop
                  ? "等待 player_open · 视频将渲染在这块黑色区域内"
                  : "浏览器预览 · 需要 Tauri 桌面环境才能播放"}
              </p>
            </div>
          )}

          {status === "loading" && (
            <div className="player-empty" aria-hidden="true">
              <p className="player-empty__text">正在准备播放器…</p>
            </div>
          )}

          {!isDesktop && hasMedia && (
            <div className="player-preview-frame">
              <span className="player-preview-frame__title">{title}</span>
              <span className="player-preview-frame__url" title={sourceUrl ?? undefined}>
                {sourceUrl}
              </span>
              <span className="player-preview-frame__hint">
                浏览器预览：控件只更新本地状态，不会调用播放器命令
              </span>
            </div>
          )}
        </div>
      </section>

      <div className="player-dock" id="player-controls">
        {controlsVisible ? (
          <section
            className="playback-panel"
            ref={controlsRef}
            aria-label="播放控制"
            onFocusCapture={clearHideTimer}
            onBlurCapture={(event) => {
              const next = event.relatedTarget;
              if (!(next instanceof Node) || !event.currentTarget.contains(next)) {
                armHideTimer();
              }
            }}
          >
            <details className="player-debug">
              <summary>调试直链</summary>
              <div className="player-source-row">
                <form
                  className="player-source"
                  noValidate
                  onSubmit={handleOpenSubmit}
                  aria-busy={isOpening}
                >
                  <span className="player-source__field">
                    <label htmlFor="player-source-url">播放地址</label>
                    <input
                      ref={urlInputRef}
                      id="player-source-url"
                      name="player-source-url"
                      type="url"
                      inputMode="url"
                      autoComplete="off"
                      spellCheck={false}
                      placeholder="https://example.com/video.mp4"
                      value={url}
                      onChange={(event) => setUrl(event.target.value)}
                    />
                  </span>
                  <button
                    className="button button--secondary player-source__submit"
                    type="submit"
                    disabled={isOpening}
                  >
                    {isOpening ? "打开中…" : "打开"}
                  </button>
                </form>
                <div className="player-source-meta">
                  <span
                    className="player-source__current"
                    title={sourceUrl ?? undefined}
                  >
                    {sourceUrl ? compactUrl(sourceUrl) : "手动输入播放直链"}
                  </span>
                  {!isDesktop && <span className="player-preview-tag">预览</span>}
                </div>
              </div>
            </details>

            {visibleError && (
              <div className="player-error" role="alert">
                <span>{visibleError}</span>
                {error && (
                  <button type="button" onClick={dismissError} aria-label="关闭错误提示">
                    ×
                  </button>
                )}
              </div>
            )}

            <div className="player-controlbar">
              <div className="player-controlbar__main">
                <button
                  ref={playButtonRef}
                  className="transport-button transport-button--main"
                  type="button"
                  onClick={() => void togglePlayback()}
                  disabled={!hasMedia}
                  aria-label={isPlaying ? "暂停" : "播放"}
                  aria-pressed={isPlaying}
                  title={!hasMedia ? "请先打开播放地址" : undefined}
                >
                  <PlayerIcon name={isPlaying ? "pause" : "play"} size={18} />
                </button>
                <span className="player-controlbar__time">
                  {formatTime(boundedPosition)}
                </span>
                <label className="visually-hidden" htmlFor="player-seek">
                  播放进度
                </label>
                <span className="range-wrap">
                  <span
                    className="range-fill"
                    style={{ width: `${progressPercent}%` }}
                    aria-hidden="true"
                  />
                  <input
                    ref={seekInputRef}
                    id="player-seek"
                    className="seek-range"
                    type="range"
                    min="0"
                    max={duration > 0 ? duration : 1}
                    step="1"
                    value={boundedPosition}
                    disabled={!canSeek}
                    aria-valuetext={`${formatTime(boundedPosition)} / ${formatTime(duration)}`}
                    onChange={(event) => updateSeekPreview(Number(event.currentTarget.value))}
                    onPointerUp={(event) => commitSeek(Number(event.currentTarget.value))}
                    onPointerCancel={(event) => commitSeek(Number(event.currentTarget.value))}
                    onKeyUp={(event) => {
                      if (
                        seekPreviewRef.current !== null &&
                        ["ArrowLeft", "ArrowRight", "Home", "End", "PageUp", "PageDown"].includes(
                          event.key,
                        )
                      ) {
                        commitSeek(Number(event.currentTarget.value));
                      }
                    }}
                    onBlur={(event) => {
                      if (seekPreviewRef.current !== null) {
                        commitSeek(Number(event.currentTarget.value));
                      }
                    }}
                  />
                </span>
                <span className="player-controlbar__time">
                  {formatTime(duration)}
                </span>
              </div>

              <div className="player-controlbar__tools">
                <TrackControl
                  kind="audio"
                  label="音轨"
                  icon="audio"
                  tracks={snapshot?.audioTracks ?? []}
                  open={trackMenu === "audio"}
                  disabled={!hasMedia || (snapshot?.audioTracks.length ?? 0) === 0}
                  triggerRef={audioTriggerRef}
                  onToggle={() => toggleTrackMenu("audio")}
                />
                <TrackControl
                  kind="subtitle"
                  label="字幕"
                  icon="captions"
                  tracks={snapshot?.subtitleTracks ?? []}
                  open={trackMenu === "subtitle"}
                  disabled={!hasMedia || (snapshot?.subtitleTracks.length ?? 0) === 0}
                  triggerRef={subtitleTriggerRef}
                  onToggle={() => toggleTrackMenu("subtitle")}
                />

                <div className="volume-control">
                  <button
                    className="tool-button"
                    type="button"
                    onClick={() => void toggleMuted()}
                    aria-label={muted ? "取消静音" : "静音"}
                    aria-pressed={muted}
                  >
                    <PlayerIcon name={muted ? "volume-off" : "volume"} size={17} />
                  </button>
                  <label className="visually-hidden" htmlFor="player-volume">
                    音量
                  </label>
                  <span className="range-wrap range-wrap--volume">
                    <span
                      className="range-fill"
                      style={{ width: `${volume}%` }}
                      aria-hidden="true"
                    />
                    <input
                      id="player-volume"
                      type="range"
                      min="0"
                      max="100"
                      step="1"
                      value={volume}
                      aria-valuetext={`${volume}%`}
                      onChange={(event) => void setVolume(Number(event.currentTarget.value))}
                    />
                  </span>
                  <span className="volume-value">{volume}</span>
                </div>

                <button
                  className="tool-button"
                  type="button"
                  onClick={() => void toggleFullscreen()}
                  aria-label={isFullscreen ? "退出全屏" : "全屏"}
                  title={isFullscreen ? "退出全屏" : "全屏"}
                >
                  <PlayerIcon name="fullscreen" size={17} />
                </button>
                <button
                  className="tool-button"
                  type="button"
                  onClick={leavePlayer}
                  aria-label="返回详情"
                  title="返回详情"
                >
                  <PlayerIcon name="arrow-left" size={17} />
                </button>
                <button
                  className="tool-button"
                  type="button"
                  aria-expanded={controlsVisible}
                  aria-controls="player-controls"
                  onClick={hideControls}
                  aria-label="隐藏控制条"
                  title="隐藏控制条"
                >
                  <PlayerIcon name="chevron" size={16} />
                </button>
              </div>
            </div>

            {trackMenu !== null && (
              <TrackPanel
                kind={trackMenu}
                label={trackMenu === "audio" ? "音轨" : "字幕"}
                tracks={
                  trackMenu === "audio"
                    ? snapshot?.audioTracks ?? []
                    : snapshot?.subtitleTracks ?? []
                }
                onSelect={(id) => handleSelectTrack(trackMenu, id)}
                onClose={closeTrackMenu}
              />
            )}

            <span className="visually-hidden" aria-live="polite">
              {label}
            </span>
          </section>
        ) : (
          <div className="player-dock--collapsed">
            <span className="player-dock__status">
              <span className={`status-square status-square--${status}`} />
              控制条已收起
            </span>
            <button
              ref={showControlsButtonRef}
              className="controls-recall"
              type="button"
              aria-expanded={false}
              aria-controls="player-controls"
              onClick={showControls}
            >
              显示控制
              <PlayerIcon name="chevron" size={15} />
            </button>
          </div>
        )}
      </div>

    </main>
  );
}
