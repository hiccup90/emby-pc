import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import type { PointerEvent as ReactPointerEvent } from "react";
import type { PlayerSnapshot, PlayerTrack } from "../player/contract";
import type { OverlayAction } from "./contract";
import {
  createPreviewSnapshot,
  dragOverlayWindow,
  fetchPlayerSnapshot,
  hasTauriRuntime,
  onOverlayWake,
  sendOverlayAction,
  setOverlayExpanded,
  setOverlayInteractive,
} from "./bridge";
import { OverlayIcon } from "./icons";
import "./overlay.css";

const AUTO_HIDE_MS = 2500;
const SNAPSHOT_POLL_MS = 250;
const SEEK_STEP_SECONDS = 10;
const SEEK_HOLD_MS = 900;
const VOLUME_HOLD_MS = 700;

function formatTime(seconds: number): string {
  const safe = Math.max(0, Math.floor(seconds));
  const hours = Math.floor(safe / 3600);
  const minutes = Math.floor((safe % 3600) / 60);
  const remaining = safe % 60;
  return hours > 0
    ? `${hours}:${String(minutes).padStart(2, "0")}:${String(remaining).padStart(2, "0")}`
    : `${minutes}:${String(remaining).padStart(2, "0")}`;
}

interface TrackOption {
  key: string;
  id: number | "no";
  title: string;
  lang: string | null;
  selected: boolean;
}

function trackOptions(
  kind: "audio" | "subtitle" | null,
  snapshot: PlayerSnapshot | null,
): TrackOption[] {
  if (kind === "audio") {
    return (snapshot?.audioTracks ?? []).map((track) => ({
      key: `audio-${track.id}`,
      id: track.id,
      title: track.title,
      lang: track.lang,
      selected: track.selected,
    }));
  }
  if (kind === "subtitle") {
    const tracks = snapshot?.subtitleTracks ?? [];
    return [
      ...tracks.map((track) => ({
        key: `subtitle-${track.id}`,
        id: track.id as number | "no",
        title: track.title,
        lang: track.lang,
        selected: track.selected,
      })),
      {
        key: "subtitle-off",
        id: "no",
        title: "关闭字幕",
        lang: null,
        selected: !tracks.some((track) => track.selected),
      },
    ];
  }
  return [];
}

interface SliderProps {
  className?: string;
  label: string;
  valueText: string;
  value: number;
  max: number;
  step?: number;
  disabled?: boolean;
  onChange: (value: number) => void;
  onCommit: (value: number) => void;
}

const COMMIT_KEYS = [
  "ArrowLeft",
  "ArrowRight",
  "ArrowUp",
  "ArrowDown",
  "Home",
  "End",
  "PageUp",
  "PageDown",
];

function Slider({
  className,
  label,
  valueText,
  value,
  max,
  step = 1,
  disabled = false,
  onChange,
  onCommit,
}: SliderProps) {
  const upper = max > 0 ? max : 1;
  const bounded = Math.min(Math.max(0, value), upper);
  const percent = (bounded / upper) * 100;

  return (
    <span className={`ov-slider${className ? ` ${className}` : ""}`}>
      <span className="ov-slider__rail" aria-hidden="true" />
      <span
        className="ov-slider__fill"
        style={{ width: `${percent}%` }}
        aria-hidden="true"
      />
      <input
        className="ov-slider__input"
        type="range"
        min={0}
        max={upper}
        step={step}
        value={bounded}
        disabled={disabled}
        aria-label={label}
        aria-valuetext={valueText}
        onChange={(event) => onChange(Number(event.currentTarget.value))}
        onPointerUp={(event) => onCommit(Number(event.currentTarget.value))}
        onPointerCancel={(event) => onCommit(Number(event.currentTarget.value))}
        onKeyUp={(event) => {
          if (COMMIT_KEYS.includes(event.key)) {
            onCommit(Number(event.currentTarget.value));
          }
        }}
        onBlur={(event) => onCommit(Number(event.currentTarget.value))}
      />
    </span>
  );
}

export default function Overlay() {
  const [desktop] = useState(hasTauriRuntime);
  const [snapshot, setSnapshot] = useState<PlayerSnapshot | null>(() =>
    hasTauriRuntime() ? null : createPreviewSnapshot(),
  );
  const [visible, setVisible] = useState(true);
  const [menu, setMenu] = useState<"audio" | "subtitle" | null>(null);
  const [seekDraft, setSeekDraft] = useState<number | null>(null);
  const [volumeDraft, setVolumeDraft] = useState<number | null>(null);
  const [activity, setActivity] = useState(0);

  const snapshotRef = useRef<PlayerSnapshot | null>(snapshot);
  const seekDraftRef = useRef<number | null>(null);
  const seekHoldRef = useRef<number | null>(null);
  const volumeHoldRef = useRef<number | null>(null);
  const lastVolumeSentRef = useRef<number | null>(null);
  const lastSeekRef = useRef<{ value: number; at: number } | null>(null);
  const lastWakeRef = useRef(0);

  const expanded = menu !== null;

  const wake = useCallback(() => {
    setVisible(true);
    const now = typeof performance !== "undefined" ? performance.now() : Date.now();
    if (now - lastWakeRef.current < 120) return;
    lastWakeRef.current = now;
    setActivity((value) => (value + 1) % 1_000_000);
  }, []);

  const send = useCallback(
    (action: OverlayAction) => {
      wake();
      if (!desktop) {
        // Standalone preview: mirror the intent locally so the bar still works.
        setSnapshot((previous) => applyPreviewAction(previous, action));
        return;
      }
      void sendOverlayAction(action).catch((reason) => {
        console.warn("[overlay] could not forward the player action", reason);
      });
    },
    [desktop, wake],
  );

  // ---- snapshot polling ----------------------------------------------------
  useEffect(() => {
    if (!desktop) return;
    let cancelled = false;
    let inFlight = false;

    const poll = () => {
      if (inFlight) return;
      inFlight = true;
      void fetchPlayerSnapshot()
        .then((next) => {
          if (cancelled || !next) return;
          snapshotRef.current = next;
          setSnapshot(next);
        })
        .catch(() => {
          // The main window surfaces player errors; polling is best-effort.
        })
        .finally(() => {
          inFlight = false;
        });
    };

    poll();
    const timer = window.setInterval(poll, SNAPSHOT_POLL_MS);
    return () => {
      cancelled = true;
      window.clearInterval(timer);
    };
  }, [desktop]);

  // ---- window geometry -----------------------------------------------------
  useEffect(() => {
    if (!desktop) return;
    void setOverlayExpanded(expanded).catch((reason) => {
      console.warn("[overlay] could not resize the overlay window", reason);
    });
  }, [desktop, expanded]);

  useEffect(() => {
    if (!desktop) return;
    void setOverlayInteractive(visible).catch((reason) => {
      console.warn("[overlay] could not toggle click-through", reason);
    });
  }, [desktop, visible]);

  useEffect(() => {
    if (!desktop) return;
    return () => {
      void setOverlayInteractive(false).catch(() => {
        // The window is closing; nothing left to report.
      });
    };
  }, [desktop]);

  // ---- wake sources --------------------------------------------------------
  useEffect(() => {
    wake();
    const onActivity = () => wake();
    const onVisibility = () => {
      if (document.visibilityState === "visible") wake();
    };
    window.addEventListener("pointermove", onActivity, { passive: true });
    window.addEventListener("pointerdown", onActivity, true);
    window.addEventListener("wheel", onActivity, { passive: true });
    window.addEventListener("focus", onActivity);
    document.addEventListener("visibilitychange", onVisibility);
    return () => {
      window.removeEventListener("pointermove", onActivity);
      window.removeEventListener("pointerdown", onActivity, true);
      window.removeEventListener("wheel", onActivity);
      window.removeEventListener("focus", onActivity);
      document.removeEventListener("visibilitychange", onVisibility);
    };
  }, [wake]);

  useEffect(() => {
    if (!desktop) return;
    let disposed = false;
    let unlisten: (() => void) | null = null;
    void onOverlayWake(wake)
      .then((stop) => {
        if (disposed) {
          stop();
          return;
        }
        unlisten = stop;
      })
      .catch((reason) => {
        console.warn("[overlay] could not listen for overlay-wake", reason);
      });
    return () => {
      disposed = true;
      unlisten?.();
    };
  }, [desktop, wake]);

  // ---- auto hide -----------------------------------------------------------
  const status = snapshot?.status ?? "idle";
  const playing = status === "playing" || status === "loading";
  const autoHideAllowed =
    status === "playing" && !snapshot?.buffering && menu === null;

  useEffect(() => {
    if (!visible || !autoHideAllowed) return;
    const timer = window.setTimeout(() => {
      setVisible(false);
    }, AUTO_HIDE_MS);
    return () => window.clearTimeout(timer);
  }, [activity, autoHideAllowed, menu, visible]);

  // ---- derived player values ----------------------------------------------
  const duration = snapshot?.durationSeconds ?? 0;
  const position = seekDraft ?? snapshot?.positionSeconds ?? 0;
  const boundedPosition = duration > 0 ? Math.min(Math.max(0, position), duration) : 0;
  const volume = volumeDraft ?? snapshot?.volume ?? 72;
  const muted = snapshot?.muted ?? false;
  const hasMedia =
    snapshot !== null && status !== "idle" && status !== "error";
  const canSeek = snapshot !== null && duration > 0 && status !== "idle" && status !== "error";
  const audioTitle =
    snapshot?.audioTracks.find((track) => track.selected)?.title ?? "默认";
  const selectedSubtitle = snapshot?.subtitleTracks.find((track) => track.selected);
  const subtitleTitle = selectedSubtitle?.title ?? "关闭";
  const errorMessage =
    status === "error" ? snapshot?.error ?? "播放错误" : null;
  const options = useMemo(() => trackOptions(menu, snapshot), [menu, snapshot]);

  // ---- slider plumbing -----------------------------------------------------
  const updateSeekDraft = (value: number | null) => {
    seekDraftRef.current = value;
    setSeekDraft(value);
  };

  const holdSeek = (value: number) => {
    if (seekHoldRef.current !== null) window.clearTimeout(seekHoldRef.current);
    updateSeekDraft(value);
    seekHoldRef.current = window.setTimeout(() => {
      seekHoldRef.current = null;
      updateSeekDraft(null);
    }, SEEK_HOLD_MS);
  };

  const commitSeek = (value: number) => {
    if (seekDraftRef.current === null) return;
    if (!Number.isFinite(value)) {
      updateSeekDraft(null);
      return;
    }
    const now = Date.now();
    const last = lastSeekRef.current;
    if (last && Math.abs(last.value - value) < 0.5 && now - last.at < 800) {
      updateSeekDraft(null);
      return;
    }
    lastSeekRef.current = { value, at: now };
    send({ type: "seek", seconds: value });
    holdSeek(value);
  };

  const changeVolume = (value: number) => {
    const next = Math.round(Math.min(100, Math.max(0, value)));
    setVolumeDraft(next);
    if (lastVolumeSentRef.current !== next) {
      lastVolumeSentRef.current = next;
      send({ type: "set-volume", value: next });
    }
    if (volumeHoldRef.current !== null) window.clearTimeout(volumeHoldRef.current);
    volumeHoldRef.current = window.setTimeout(() => {
      volumeHoldRef.current = null;
      lastVolumeSentRef.current = null;
      setVolumeDraft(null);
    }, VOLUME_HOLD_MS);
  };

  useEffect(() => {
    return () => {
      if (seekHoldRef.current !== null) window.clearTimeout(seekHoldRef.current);
      if (volumeHoldRef.current !== null) window.clearTimeout(volumeHoldRef.current);
    };
  }, []);

  // ---- interactions --------------------------------------------------------
  const toggleMenu = (kind: "audio" | "subtitle") => {
    wake();
    setVisible(true);
    setMenu((current) => (current === kind ? null : kind));
  };

  const selectTrack = (option: TrackOption) => {
    if (menu === null) return;
    send({ type: "set-track", kind: menu, id: option.id });
    setMenu(null);
  };

  const handleDragStart = (event: ReactPointerEvent<HTMLDivElement>) => {
    if (event.button !== 0) return;
    const target = event.target;
    if (
      target instanceof HTMLElement &&
      target.closest("button, input, label, summary, a, select, textarea")
    ) {
      return;
    }
    if (!desktop) return;
    void dragOverlayWindow().catch(() => {
      // Dragging is a Windows nicety; a failure must not break the bar.
    });
  };

  // ---- overlay keyboard ----------------------------------------------------
  useEffect(() => {
    const onKeyDown = (event: KeyboardEvent) => {
      if (event.defaultPrevented) return;
      wake();
      const target = event.target;

      if (event.key === "Escape") {
        if (menu !== null) {
          event.preventDefault();
          setMenu(null);
          return;
        }
        event.preventDefault();
        send({ type: "back" });
        return;
      }

      if (
        target instanceof HTMLElement &&
        Boolean(target.closest("button, a, [role='menu'], [role='menuitemradio']"))
      ) {
        return;
      }
      if (event.ctrlKey || event.metaKey || event.altKey) return;

      if (event.code === "Space") {
        event.preventDefault();
        send({ type: "toggle-play" });
        return;
      }
      if (event.key === "ArrowRight" || event.key === "ArrowLeft") {
        const current = snapshotRef.current;
        if (!current || current.durationSeconds <= 0) return;
        event.preventDefault();
        const delta = event.key === "ArrowRight" ? SEEK_STEP_SECONDS : -SEEK_STEP_SECONDS;
        send({
          type: "seek",
          seconds: Math.min(
            Math.max(0, current.positionSeconds + delta),
            current.durationSeconds,
          ),
        });
        return;
      }
      if (event.key.toLowerCase() === "m") {
        event.preventDefault();
        send({ type: "toggle-mute" });
        return;
      }
      if (event.key.toLowerCase() === "f") {
        event.preventDefault();
        send({ type: "toggle-fullscreen" });
      }
    };

    window.addEventListener("keydown", onKeyDown);
    return () => window.removeEventListener("keydown", onKeyDown);
  }, [menu, send, wake]);

  const menuLabel = menu === "audio" ? "音轨" : "字幕";

  return (
    <div
      className="overlay"
      data-visible={visible ? "true" : "false"}
      data-expanded={expanded ? "true" : "false"}
    >
      {expanded && (
        <div className="overlay__panel" onPointerDown={handleDragStart}>
          <div className="overlay__menus">
            {menu !== null && (
              <section className="ov-menu" aria-label={`${menuLabel}选择`}>
                <div className="ov-menu__head">
                  <span>{menuLabel}</span>
                  <button
                    className="ov-menu__close"
                    type="button"
                    onClick={() => setMenu(null)}
                    aria-label={`关闭${menuLabel}菜单`}
                  >
                    关闭
                  </button>
                </div>
                <div
                  className="ov-menu__list"
                  role="menu"
                  aria-label={`选择${menuLabel}`}
                >
                  {options.length === 0 ? (
                    <span className="ov-menu__empty">暂无可用轨道</span>
                  ) : (
                    options.map((option) => (
                      <button
                        key={option.key}
                        className={`ov-menu__option${option.selected ? " is-current" : ""}`}
                        type="button"
                        role="menuitemradio"
                        aria-checked={option.selected}
                        aria-label={
                          option.lang ? `${option.title}（${option.lang}）` : option.title
                        }
                        onClick={() => selectTrack(option)}
                      >
                        <span className="ov-menu__title">
                          {option.title}
                          {option.lang && <small>{option.lang}</small>}
                        </span>
                        {option.selected && (
                          <span className="ov-menu__check" aria-hidden="true">
                            ✓
                          </span>
                        )}
                      </button>
                    ))
                  )}
                </div>
              </section>
            )}
          </div>
        </div>
      )}

      <div className="overlay__bar" onPointerDown={handleDragStart}>
        <button
          className="ov-btn ov-btn--play"
          type="button"
          onClick={() => send({ type: "toggle-play" })}
          disabled={!hasMedia}
          aria-label={playing ? "暂停" : "播放"}
          aria-pressed={playing}
        >
          <OverlayIcon name={playing ? "pause" : "play"} size={18} />
        </button>

        <span className="ov-time" aria-hidden="true">
          {formatTime(boundedPosition)}
        </span>

        <Slider
          className="ov-slider--seek"
          label="播放进度"
          valueText={`${formatTime(boundedPosition)} / ${formatTime(duration)}`}
          value={boundedPosition}
          max={duration}
          disabled={!canSeek}
          onChange={(value) => updateSeekDraft(value)}
          onCommit={commitSeek}
        />

        <span className="ov-time" aria-hidden="true">
          {formatTime(duration)}
        </span>

        <span className="ov-spacer" aria-hidden="true" />

        {errorMessage && (
          <>
            <span className="ov-error" role="alert" title={errorMessage}>
              {errorMessage}
            </span>
            <button
              className="ov-btn"
              type="button"
              onClick={() => send({ type: "retry" })}
              aria-label="重试"
            >
              <OverlayIcon name="retry" size={16} />
            </button>
          </>
        )}

        <button
          className={`ov-btn${menu === "audio" ? " is-active" : ""}`}
          type="button"
          onClick={() => toggleMenu("audio")}
          disabled={!hasMedia || (snapshot?.audioTracks.length ?? 0) === 0}
          aria-label={`音轨：${audioTitle}`}
          aria-haspopup="menu"
          aria-expanded={menu === "audio"}
        >
          <OverlayIcon name="audio" />
        </button>

        <button
          className={`ov-btn${menu === "subtitle" ? " is-active" : ""}`}
          type="button"
          onClick={() => toggleMenu("subtitle")}
          disabled={!hasMedia || (snapshot?.subtitleTracks.length ?? 0) === 0}
          aria-label={`字幕：${subtitleTitle}`}
          aria-haspopup="menu"
          aria-expanded={menu === "subtitle"}
        >
          <OverlayIcon name="captions" />
        </button>

        <button
          className="ov-btn"
          type="button"
          onClick={() => send({ type: "toggle-mute" })}
          disabled={!snapshot}
          aria-label={muted ? "取消静音" : "静音"}
          aria-pressed={muted}
        >
          <OverlayIcon name={muted ? "volume-off" : "volume"} />
        </button>

        <Slider
          className="ov-slider--volume"
          label="音量"
          valueText={`${volume}%`}
          value={volume}
          max={100}
          disabled={!snapshot}
          onChange={changeVolume}
          onCommit={changeVolume}
        />

        <button
          className="ov-btn"
          type="button"
          onClick={() => send({ type: "toggle-fullscreen" })}
          aria-label="切换全屏"
          title="切换全屏"
        >
          <OverlayIcon name="fullscreen" />
        </button>

        <button
          className="ov-btn"
          type="button"
          onClick={() => send({ type: "back" })}
          aria-label="返回详情"
          title="返回详情"
        >
          <OverlayIcon name="back" />
        </button>
      </div>
    </div>
  );
}

/** Standalone (non-Tauri) preview keeps the bar interactive without Rust. */
function applyPreviewAction(
  previous: PlayerSnapshot | null,
  action: OverlayAction,
): PlayerSnapshot | null {
  if (!previous) return previous;
  const next = { ...previous, seq: previous.seq + 1 };
  switch (action.type) {
    case "toggle-play":
      return {
        ...next,
        status: previous.status === "playing" ? "paused" : "playing",
        buffering: false,
      };
    case "seek":
      return {
        ...next,
        positionSeconds: Math.min(
          Math.max(0, action.seconds),
          previous.durationSeconds,
        ),
      };
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
        selected:
          action.id === "no" ? false : track.id === action.id,
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
      return next;
  }
}
