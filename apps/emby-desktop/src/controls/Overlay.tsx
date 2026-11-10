import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import type {
  CSSProperties,
  KeyboardEvent as ReactKeyboardEvent,
  PointerEvent as ReactPointerEvent,
} from "react";
import type { PlaybackMeta, PlayerSnapshot, PlayerTrack } from "../player/contract";
import type { OverlayAction } from "./contract";
import {
  createPreviewMeta,
  createPreviewSnapshot,
  dragOverlayWindow,
  fetchPlayerSnapshot,
  hasTauriRuntime,
  onOverlayMeta,
  onOverlayWake,
  requestOverlayMeta,
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
/** Transport buttons, as in Plex: back 10 s, forward 30 s. */
const SKIP_BACK_SECONDS = 10;
const SKIP_FORWARD_SECONDS = 30;

function formatTime(seconds: number): string {
  const safe = Math.max(0, Math.floor(seconds));
  const hours = Math.floor(safe / 3600);
  const minutes = Math.floor((safe % 3600) / 60);
  const remaining = safe % 60;
  return hours > 0
    ? `${hours}:${String(minutes).padStart(2, "0")}:${String(remaining).padStart(2, "0")}`
    : `${minutes}:${String(remaining).padStart(2, "0")}`;
}

function formatError(reason: unknown): string {
  if (reason instanceof Error) return reason.message;
  if (typeof reason === "string") return reason;
  try {
    return JSON.stringify(reason);
  } catch {
    return String(reason);
  }
}

/**
 * NOACTIVATE windows can drop the composed `click` (focus/capture hand-off),
 * so an action must run on the press itself. The press is also swallowed so a
 * later `click` cannot repeat it and the bar drag handler never sees it.
 */
function actionPointerDown(run: () => void) {
  return (event: ReactPointerEvent<HTMLButtonElement>) => {
    if (event.currentTarget.disabled) return;
    if (event.button !== 0 || !event.isPrimary) return;
    event.preventDefault();
    event.stopPropagation();
    run();
  };
}

/** Keyboard users still get the action once: the native click is suppressed. */
function actionKeyDown(run: () => void) {
  return (event: ReactKeyboardEvent<HTMLButtonElement>) => {
    if (event.currentTarget.disabled) return;
    if (event.key !== "Enter" && event.key !== " " && event.key !== "Spacebar") {
      return;
    }
    event.preventDefault();
    event.stopPropagation();
    if (event.repeat) return;
    run();
  };
}

function swallowClick(event: { preventDefault: () => void }) {
  event.preventDefault();
}

/** Readable names for the ISO 639 codes mpv reports when a track has no title. */
const LANGUAGE_NAMES: Record<string, string> = {
  chi: "中文",
  zho: "中文",
  zh: "中文",
  chs: "简体中文",
  cht: "繁體中文",
  eng: "English",
  en: "English",
  jpn: "日本語",
  ja: "日本語",
  kor: "한국어",
  ko: "한국어",
  fre: "Français",
  fra: "Français",
  fr: "Français",
  ger: "Deutsch",
  deu: "Deutsch",
  de: "Deutsch",
  spa: "Español",
  es: "Español",
  ita: "Italiano",
  it: "Italiano",
  rus: "Русский",
  ru: "Русский",
  por: "Português",
  pt: "Português",
  tha: "ไทย",
  th: "ไทย",
};

/** Row label for a track: its own title, else the language, else a number. */
function trackLabel(
  track: Pick<PlayerTrack, "id" | "title" | "lang">,
  fallback: string,
): { title: string; detail: string | null } {
  const title = (track.title ?? "").trim();
  const code = track.lang?.trim().toLowerCase() || null;
  const language = code ? LANGUAGE_NAMES[code] ?? null : null;
  if (title) return { title, detail: code ? code.toUpperCase() : null };
  if (language) return { title: language, detail: null };
  if (code) return { title: code.toUpperCase(), detail: null };
  return { title: `${fallback} ${track.id}`, detail: null };
}

interface TrackOption {
  key: string;
  id: number | "no";
  title: string;
  /** Muted language code next to the title. */
  detail: string | null;
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
      ...trackLabel(track, "音轨"),
      selected: track.selected,
    }));
  }
  if (kind === "subtitle") {
    const tracks = snapshot?.subtitleTracks ?? [];
    // "Off" first, as in Plex's subtitle list.
    return [
      {
        key: "subtitle-off",
        id: "no",
        title: "关闭字幕",
        detail: null,
        selected: !tracks.some((track) => track.selected),
      },
      ...tracks.map((track) => ({
        key: `subtitle-${track.id}`,
        id: track.id as number | "no",
        ...trackLabel(track, "字幕"),
        selected: track.selected,
      })),
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
  onGestureStart: () => void;
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
  onGestureStart,
}: SliderProps) {
  const upper = max > 0 ? max : 1;
  const bounded = Math.min(Math.max(0, value), upper);
  const percent = (bounded / upper) * 100;
  // One commit per gesture: the pointerup commit must not be repeated by the
  // blur that happens when the user clicks elsewhere a moment later.
  const dirtyRef = useRef(false);
  const lastCommittedRef = useRef<number | null>(null);

  const commit = (input: HTMLInputElement) => {
    const next = Number(input.value);
    lastCommittedRef.current = next;
    dirtyRef.current = false;
    onCommit(next);
  };

  const commitIfDirty = (input: HTMLInputElement) => {
    if (!dirtyRef.current) return;
    if (lastCommittedRef.current === Number(input.value)) {
      dirtyRef.current = false;
      return;
    }
    commit(input);
  };

  return (
    <span
      className={`ov-slider${className ? ` ${className}` : ""}`}
      style={{ "--ov-p": percent / 100 } as CSSProperties}
      data-no-drag="true"
    >
      <span className="ov-slider__rail" aria-hidden="true" />
      <span className="ov-slider__fill" aria-hidden="true" />
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
        onChange={(event) => {
          dirtyRef.current = true;
          onChange(Number(event.currentTarget.value));
        }}
        onPointerDown={(event) => {
          if (event.button !== 0 || !event.isPrimary) return;
          // Do not preventDefault: the native range thumb still has to drag.
          event.stopPropagation();
          // Wake before capture so the window cannot turn click-through in the
          // middle of a drag and lose pointermove/pointerup.
          onGestureStart();
          try {
            event.currentTarget.setPointerCapture(event.pointerId);
          } catch {
            // The pointer may already be gone; native dragging still works.
          }
        }}
        onPointerUp={(event) => {
          if (event.currentTarget.hasPointerCapture(event.pointerId)) {
            event.currentTarget.releasePointerCapture(event.pointerId);
          }
          commitIfDirty(event.currentTarget);
        }}
        onPointerCancel={(event) => {
          if (event.currentTarget.hasPointerCapture(event.pointerId)) {
            event.currentTarget.releasePointerCapture(event.pointerId);
          }
          commitIfDirty(event.currentTarget);
        }}
        onKeyUp={(event) => {
          if (COMMIT_KEYS.includes(event.key)) {
            commitIfDirty(event.currentTarget);
          }
        }}
        onBlur={(event) => commitIfDirty(event.currentTarget)}
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
  const [sendError, setSendError] = useState<string | null>(null);
  const [meta, setMeta] = useState<PlaybackMeta | null>(() =>
    hasTauriRuntime() ? null : createPreviewMeta(),
  );

  const snapshotRef = useRef<PlayerSnapshot | null>(snapshot);
  const visibleRef = useRef(true);
  const autoHideTimerRef = useRef<number | null>(null);
  const autoHideAllowedRef = useRef(false);
  const lastInteractiveRef = useRef<boolean | null>(null);
  const seekDraftRef = useRef<number | null>(null);
  const seekHoldRef = useRef<number | null>(null);
  const volumeHoldRef = useRef<number | null>(null);
  const lastVolumeSentRef = useRef<number | null>(null);
  const lastSeekRef = useRef<{ value: number; at: number } | null>(null);

  const expanded = menu !== null;

  const clearAutoHide = useCallback(() => {
    if (autoHideTimerRef.current !== null) {
      window.clearTimeout(autoHideTimerRef.current);
      autoHideTimerRef.current = null;
    }
  }, []);

  const hideOverlay = useCallback(() => {
    clearAutoHide();
    if (!visibleRef.current) return;
    visibleRef.current = false;
    setVisible(false);
  }, [clearAutoHide]);

  // Only real state changes (playing/buffering/menu and visibility) arm the
  // timer. It lives in a ref so the 250 ms snapshot poll cannot rebuild it and
  // restart the fade on every render.
  const scheduleAutoHide = useCallback(() => {
    clearAutoHide();
    if (!visibleRef.current || !autoHideAllowedRef.current) return;
    autoHideTimerRef.current = window.setTimeout(() => {
      autoHideTimerRef.current = null;
      hideOverlay();
    }, AUTO_HIDE_MS);
  }, [clearAutoHide, hideOverlay]);

  const wake = useCallback(() => {
    // Already visible: just push the hide deadline out. Do not touch CSS
    // animation state or remount nodes, so repeated wakes cannot flicker.
    if (!visibleRef.current) {
      visibleRef.current = true;
      setVisible(true);
    }
    scheduleAutoHide();
  }, [scheduleAutoHide]);

  const send = useCallback(
    (action: OverlayAction) => {
      wake();
      if (!desktop) {
        // Standalone preview: mirror the intent locally so the bar still works.
        setSnapshot((previous) => applyPreviewAction(previous, action));
        return;
      }
      void sendOverlayAction(action)
        .then(() => {
          // Delivered: clear a previous failure hint (no-op when already clear).
          setSendError(null);
        })
        .catch((reason) => {
          console.warn("[overlay] could not forward the player action", reason);
          // Real-machine diagnosis: without a visible hint a dropped IPC looks
          // exactly like a button that does not work.
          setSendError(`控制指令未送达：${formatError(reason)}`);
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
      console.warn("[overlay] could not expand/collapse the overlay window", reason);
    });
  }, [desktop, expanded]);

  useEffect(() => {
    if (!desktop) return;
    // `visible` is the real state: skip duplicate IPC when it did not change.
    if (lastInteractiveRef.current === visible) return;
    lastInteractiveRef.current = visible;
    void setOverlayInteractive(visible).catch((reason) => {
      console.warn("[overlay] could not toggle click-through", reason);
    });
  }, [desktop, visible]);

  useEffect(() => {
    if (!desktop) return;
    return () => {
      if (autoHideTimerRef.current !== null) {
        window.clearTimeout(autoHideTimerRef.current);
        autoHideTimerRef.current = null;
      }
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

  // ---- title row ------------------------------------------------------------
  // The player page pushes PlaybackMeta; a late or reloaded controls window
  // asks for it once its listener is in place.
  useEffect(() => {
    if (!desktop) return;
    let disposed = false;
    let lastStamp = 0;
    let unlisten: (() => void) | null = null;
    void onOverlayMeta((message) => {
      if (message.stamp <= lastStamp) return;
      lastStamp = message.stamp;
      setMeta(message.meta);
    })
      .then((stop) => {
        if (disposed) {
          stop();
          return;
        }
        unlisten = stop;
        return requestOverlayMeta();
      })
      .catch((reason) => {
        console.warn("[overlay] could not sync the title row", reason);
      });
    return () => {
      disposed = true;
      unlisten?.();
    };
  }, [desktop]);

  // ---- auto hide -----------------------------------------------------------
  const status = snapshot?.status ?? "idle";
  const playing = status === "playing" || status === "loading";
  const buffering = snapshot?.buffering ?? false;
  const autoHideAllowed = status === "playing" && !buffering && menu === null;

  useEffect(() => {
    autoHideAllowedRef.current = autoHideAllowed;
    scheduleAutoHide();
  }, [autoHideAllowed, scheduleAutoHide]);

  // ---- derived player values ----------------------------------------------
  const duration = snapshot?.durationSeconds ?? 0;
  const position = seekDraft ?? snapshot?.positionSeconds ?? 0;
  const boundedPosition = duration > 0 ? Math.min(Math.max(0, position), duration) : 0;
  const volume = volumeDraft ?? snapshot?.volume ?? 72;
  const muted = snapshot?.muted ?? false;
  const hasMedia =
    snapshot !== null && status !== "idle" && status !== "error";
  const canSeek = snapshot !== null && duration > 0 && status !== "idle" && status !== "error";
  const selectedAudio = snapshot?.audioTracks.find((track) => track.selected);
  const audioTitle = selectedAudio ? trackLabel(selectedAudio, "音轨").title : "默认";
  const selectedSubtitle = snapshot?.subtitleTracks.find((track) => track.selected);
  const subtitleTitle = selectedSubtitle
    ? trackLabel(selectedSubtitle, "字幕").title
    : "关闭";
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
    setMenu((current) => (current === kind ? null : kind));
  };

  const selectTrack = (option: TrackOption) => {
    if (menu === null) return;
    send({ type: "set-track", kind: menu, id: option.id });
    setMenu(null);
  };

  /** Transport skip. Steps from a pending seek so quick presses accumulate. */
  const skip = (delta: number) => {
    if (!snapshot || snapshot.durationSeconds <= 0) return;
    const from = seekDraftRef.current ?? snapshot.positionSeconds;
    const target = Math.min(Math.max(0, from + delta), snapshot.durationSeconds);
    send({ type: "seek", seconds: target });
    holdSeek(target);
  };

  /** A press on the empty strip above the bar closes the popover (Plex-like). */
  const handlePanelPointerDown = (event: ReactPointerEvent<HTMLDivElement>) => {
    if (event.button !== 0 || !event.isPrimary) return;
    const target = event.target;
    if (target instanceof Element && target.closest(".ov-menu")) return;
    event.preventDefault();
    setMenu(null);
  };

  const handleDragStart = (event: ReactPointerEvent<HTMLDivElement>) => {
    if (event.button !== 0 || !event.isPrimary || event.defaultPrevented) return;
    // Chromium still targets `pointerdown` at a disabled button (only the mouse
    // events are suppressed), hence the `button` selector below.
    const target = event.target;
    if (!(target instanceof Element)) return;
    if (
      target.closest(
        "[data-no-drag], button, input, label, summary, a, select, textarea, [role='menu'], [role='menuitemradio']",
      )
    ) {
      return;
    }
    if (!desktop) return;
    void dragOverlayWindow().catch((reason) => {
      console.warn("[overlay] could not start the window drag", reason);
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
  const displayError = errorMessage ?? sendError;
  const timeText =
    duration > 0 ? `${formatTime(boundedPosition)} / ${formatTime(duration)}` : null;
  const statusText =
    status === "loading" ? "正在载入…" : buffering && status === "playing" ? "缓冲中" : null;
  const titleText = meta?.title || null;
  const secondLine = displayError ?? meta?.subtitle ?? null;
  const thirdLine = [statusText, timeText].filter(Boolean).join(" · ") || null;
  const volumeIcon =
    muted || volume === 0 ? "volume-off" : volume < 50 ? "volume-low" : "volume";

  return (
    <div
      className="overlay"
      data-visible={visible ? "true" : "false"}
      data-expanded={expanded ? "true" : "false"}
      aria-hidden={!visible}
    >
      <div
        className="overlay__panel"
        data-open={expanded ? "true" : "false"}
        aria-hidden={!expanded}
        onPointerDown={handlePanelPointerDown}
      >
        {menu !== null && (
          <section className="ov-menu" aria-label={`${menuLabel}选择`} data-no-drag="true">
            <div className="ov-menu__head">
              <span className="ov-menu__heading">{menuLabel}</span>
              <button
                className="ov-menu__close"
                type="button"
                data-no-drag="true"
                onPointerDown={actionPointerDown(() => setMenu(null))}
                onKeyDown={actionKeyDown(() => setMenu(null))}
                onClick={swallowClick}
                aria-label={`关闭${menuLabel}菜单`}
              >
                <OverlayIcon name="close" size={16} />
              </button>
            </div>
            <div className="ov-menu__list" role="menu" aria-label={`选择${menuLabel}`}>
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
                      option.detail ? `${option.title}（${option.detail}）` : option.title
                    }
                    data-no-drag="true"
                    onPointerDown={actionPointerDown(() => selectTrack(option))}
                    onKeyDown={actionKeyDown(() => selectTrack(option))}
                    onClick={swallowClick}
                  >
                    <span className="ov-menu__title">{option.title}</span>
                    {option.detail && (
                      <span className="ov-menu__detail">{option.detail}</span>
                    )}
                    <span className="ov-menu__check" aria-hidden="true">
                      {option.selected && <OverlayIcon name="check" size={16} />}
                    </span>
                  </button>
                ))
              )}
            </div>
          </section>
        )}
      </div>

      <div className="overlay__bar" onPointerDown={handleDragStart}>
        <div className="ov-bar__band" aria-hidden="true" />

        <div className="ov-seek">
          <Slider
            className="ov-slider--seek"
            label="播放进度"
            valueText={`${formatTime(boundedPosition)} / ${formatTime(duration)}`}
            value={boundedPosition}
            max={duration}
            disabled={!canSeek}
            onChange={(value) => updateSeekDraft(value)}
            onCommit={commitSeek}
            onGestureStart={wake}
          />
        </div>

        <div className="ov-controls">
          <div className="ov-group ov-group--left">
            <button
              className="ov-btn"
              type="button"
              data-no-drag="true"
              onPointerDown={actionPointerDown(() => send({ type: "back" }))}
              onKeyDown={actionKeyDown(() => send({ type: "back" }))}
              onClick={swallowClick}
              aria-label="返回详情"
              title="返回详情"
            >
              <OverlayIcon name="back" size={22} />
            </button>
            <div
              className="ov-meta"
              title={[titleText, secondLine].filter(Boolean).join("\n") || undefined}
            >
              {titleText && <span className="ov-meta__title">{titleText}</span>}
              {secondLine && (
                <span
                  className={`ov-meta__sub${displayError ? " is-error" : ""}`}
                  role={displayError ? "alert" : undefined}
                >
                  {secondLine}
                </span>
              )}
              {thirdLine && (
                <span className="ov-meta__time">
                  {statusText && <span className="ov-spinner" aria-hidden="true" />}
                  {thirdLine}
                </span>
              )}
            </div>
          </div>

          <div className="ov-group ov-group--center">
            <button
              className="ov-btn"
              type="button"
              data-no-drag="true"
              onPointerDown={actionPointerDown(() => skip(-SKIP_BACK_SECONDS))}
              onKeyDown={actionKeyDown(() => skip(-SKIP_BACK_SECONDS))}
              onClick={swallowClick}
              disabled={!canSeek}
              aria-label={`后退 ${SKIP_BACK_SECONDS} 秒`}
            >
              <OverlayIcon name="replay-10" size={24} />
            </button>
            {errorMessage ? (
              <button
                key="retry"
                className="ov-btn ov-btn--play"
                type="button"
                data-no-drag="true"
                onPointerDown={actionPointerDown(() => send({ type: "retry" }))}
                onKeyDown={actionKeyDown(() => send({ type: "retry" }))}
                onClick={swallowClick}
                aria-label="重试"
                title="重试"
              >
                <OverlayIcon name="retry" size={20} />
              </button>
            ) : (
              <button
                key="play"
                className="ov-btn ov-btn--play"
                type="button"
                data-no-drag="true"
                onPointerDown={actionPointerDown(() => send({ type: "toggle-play" }))}
                onKeyDown={actionKeyDown(() => send({ type: "toggle-play" }))}
                onClick={swallowClick}
                disabled={!hasMedia}
                aria-label={playing ? "暂停" : "播放"}
                aria-pressed={playing}
              >
                <OverlayIcon name={playing ? "pause" : "play"} size={22} />
              </button>
            )}
            <button
              className="ov-btn"
              type="button"
              data-no-drag="true"
              onPointerDown={actionPointerDown(() => skip(SKIP_FORWARD_SECONDS))}
              onKeyDown={actionKeyDown(() => skip(SKIP_FORWARD_SECONDS))}
              onClick={swallowClick}
              disabled={!canSeek}
              aria-label={`前进 ${SKIP_FORWARD_SECONDS} 秒`}
            >
              <OverlayIcon name="forward-30" size={24} />
            </button>
          </div>

          <div className="ov-group ov-group--right">
            <button
              className="ov-btn"
              type="button"
              data-no-drag="true"
              onPointerDown={actionPointerDown(() => send({ type: "toggle-mute" }))}
              onKeyDown={actionKeyDown(() => send({ type: "toggle-mute" }))}
              onClick={swallowClick}
              disabled={!snapshot}
              aria-label={muted ? "取消静音" : "静音"}
              aria-pressed={muted}
            >
              <OverlayIcon name={volumeIcon} />
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
              onGestureStart={wake}
            />
            <button
              className={`ov-btn${menu === "audio" ? " is-active" : ""}`}
              type="button"
              data-no-drag="true"
              onPointerDown={actionPointerDown(() => toggleMenu("audio"))}
              onKeyDown={actionKeyDown(() => toggleMenu("audio"))}
              onClick={swallowClick}
              disabled={!hasMedia || (snapshot?.audioTracks.length ?? 0) === 0}
              aria-label={`音轨：${audioTitle}`}
              title="音轨"
              aria-haspopup="menu"
              aria-expanded={menu === "audio"}
            >
              <OverlayIcon name="audio" />
            </button>
            <button
              className={`ov-btn${menu === "subtitle" ? " is-active" : ""}`}
              type="button"
              data-no-drag="true"
              onPointerDown={actionPointerDown(() => toggleMenu("subtitle"))}
              onKeyDown={actionKeyDown(() => toggleMenu("subtitle"))}
              onClick={swallowClick}
              disabled={!hasMedia || (snapshot?.subtitleTracks.length ?? 0) === 0}
              aria-label={`字幕：${subtitleTitle}`}
              title="字幕"
              aria-haspopup="menu"
              aria-expanded={menu === "subtitle"}
            >
              <OverlayIcon name="subtitles" />
            </button>
            <button
              className="ov-btn"
              type="button"
              data-no-drag="true"
              onPointerDown={actionPointerDown(() => send({ type: "toggle-fullscreen" }))}
              onKeyDown={actionKeyDown(() => send({ type: "toggle-fullscreen" }))}
              onClick={swallowClick}
              aria-label="切换全屏"
              title="切换全屏"
            >
              <OverlayIcon name="fullscreen" />
            </button>
          </div>
        </div>
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
