import { useCallback, useEffect, useRef, useState } from "react";
import type { PlayerSnapshot, PlayerTrack } from "./contract";
import {
  createPlayerChannel,
  describeError,
  hasTauriRuntime,
  invokePlayerOpen,
  invokePlayerSeek,
  invokePlayerSetMuted,
  invokePlayerSetPaused,
  invokePlayerSetTrack,
  invokePlayerSetVolume,
  invokePlayerSubscribe,
  invokePlayerToggleFullscreen,
} from "./tauri";

type PlayerTrackKind = PlayerTrack["type"];

const PREVIEW_DURATION_SECONDS = 96 * 60;

function createIdleSnapshot(): PlayerSnapshot {
  return {
    seq: 0,
    status: "idle",
    positionSeconds: 0,
    durationSeconds: 0,
    volume: 72,
    muted: false,
    buffering: false,
    videoWidth: 0,
    videoHeight: 0,
    audioTracks: [],
    subtitleTracks: [],
    error: null,
  };
}

function createPreviewSnapshot(
  volume: number,
  muted: boolean,
  startSeconds = 0,
): PlayerSnapshot {
  return {
    seq: 1,
    status: "playing",
    positionSeconds: Math.min(Math.max(0, startSeconds), PREVIEW_DURATION_SECONDS),
    durationSeconds: PREVIEW_DURATION_SECONDS,
    volume,
    muted,
    buffering: false,
    videoWidth: 1920,
    videoHeight: 1080,
    audioTracks: [
      { id: 1, type: "audio", title: "国语 · 立体声", lang: "zh", selected: true },
      { id: 2, type: "audio", title: "原声 · 5.1", lang: "en", selected: false },
    ],
    subtitleTracks: [
      { id: 1, type: "subtitle", title: "简体中文", lang: "zh", selected: true },
      { id: 2, type: "subtitle", title: "English", lang: "en", selected: false },
    ],
    error: null,
  };
}

function normalizeSourceUrl(rawUrl: string): string {
  const value = rawUrl.trim();
  if (!value) {
    throw new Error("请输入播放地址。");
  }
  try {
    return new URL(value).toString();
  } catch {
    throw new Error("播放地址无效，请输入完整 URL（例如 https://example.com/video.mp4）。");
  }
}

export function usePlayer() {
  const isDesktop = hasTauriRuntime();
  const [snapshot, setSnapshot] = useState<PlayerSnapshot | null>(() =>
    isDesktop ? null : createIdleSnapshot(),
  );
  const [error, setError] = useState<string | null>(null);
  const [sourceUrl, setSourceUrl] = useState<string | null>(null);
  const [isFullscreen, setIsFullscreen] = useState(false);
  const mountedRef = useRef(true);
  const lastSeqRef = useRef(-1);

  useEffect(() => {
    mountedRef.current = true;
    return () => {
      mountedRef.current = false;
    };
  }, []);

  const acceptSnapshot = useCallback((next: PlayerSnapshot) => {
    if (!mountedRef.current) return;
    if (next.seq < lastSeqRef.current) return;
    lastSeqRef.current = next.seq;
    setSnapshot(next);
  }, []);

  useEffect(() => {
    if (!isDesktop) return;

    let cancelled = false;
    const channel = createPlayerChannel((next) => {
      if (!cancelled) acceptSnapshot(next);
    });

    void invokePlayerSubscribe(channel).catch((reason: unknown) => {
      if (!cancelled) {
        setError(describeError(reason, "无法订阅播放器状态"));
      }
    });

    // `player_subscribe` delivers the current snapshot to the new sink right
    // away (SnapshotHub::set_sink), so no extra `player_snapshot` fetch is owed.

    return () => {
      cancelled = true;
    };
  }, [acceptSnapshot, isDesktop]);

  useEffect(() => {
    if (isDesktop || snapshot?.status !== "playing") return;
    const timer = window.setInterval(() => {
      setSnapshot((previous) => {
        if (!previous || previous.status !== "playing") return previous;
        const position = Math.min(previous.durationSeconds, previous.positionSeconds + 1);
        const ended = previous.durationSeconds > 0 && position >= previous.durationSeconds;
        return {
          ...previous,
          seq: previous.seq + 1,
          positionSeconds: position,
          status: ended ? "ended" : "playing",
          buffering: ended ? false : previous.buffering,
        };
      });
    }, 1000);
    return () => window.clearInterval(timer);
  }, [isDesktop, snapshot?.status]);

  useEffect(() => {
    if (isDesktop) return;
    const onChange = () => setIsFullscreen(Boolean(document.fullscreenElement));
    document.addEventListener("fullscreenchange", onChange);
    return () => document.removeEventListener("fullscreenchange", onChange);
  }, [isDesktop]);

  const open = useCallback(
    async (rawUrl: string, startSeconds = 0): Promise<boolean> => {
      let url: string;
      try {
        url = normalizeSourceUrl(rawUrl);
      } catch (reason) {
        setError(describeError(reason, "播放地址无效"));
        return false;
      }

      const resumeAt = Number.isFinite(startSeconds) ? Math.max(0, startSeconds) : 0;
      setError(null);
      if (!isDesktop) {
        setSnapshot((previous) => {
          const base = createPreviewSnapshot(
            previous?.volume ?? 72,
            previous?.muted ?? false,
            resumeAt,
          );
          return { ...base, seq: (previous?.seq ?? 0) + 1 };
        });
        setSourceUrl(url);
        return true;
      }

      lastSeqRef.current = -1;
      try {
        const next = await invokePlayerOpen({ url, startSeconds: resumeAt });
        if (mountedRef.current) {
          setSourceUrl(url);
          if (next.seq >= lastSeqRef.current) {
            lastSeqRef.current = next.seq;
            setSnapshot(next);
          }
        }
        return true;
      } catch (reason) {
        if (mountedRef.current) {
          setError(describeError(reason, "无法打开播放地址"));
        }
        return false;
      }
    },
    [isDesktop],
  );

  const togglePlayback = useCallback(async () => {
    if (!snapshot || snapshot.status === "idle" || snapshot.status === "error") {
      setError("请先输入并打开播放地址。");
      return;
    }

    const nextPaused = snapshot.status === "playing" || snapshot.status === "loading";
    setError(null);

    if (!isDesktop) {
      setSnapshot((previous) =>
        previous
          ? {
              ...previous,
              seq: previous.seq + 1,
              status: nextPaused ? "paused" : "playing",
              buffering: false,
            }
          : previous,
      );
      return;
    }

    try {
      await invokePlayerSetPaused(nextPaused);
      if (!mountedRef.current) return;
      setSnapshot((previous) =>
        previous ? { ...previous, status: nextPaused ? "paused" : "playing" } : previous,
      );
    } catch (reason) {
      if (mountedRef.current) {
        setError(describeError(reason, nextPaused ? "暂停失败" : "播放失败"));
      }
    }
  }, [isDesktop, snapshot]);

  const seek = useCallback(
    async (seconds: number): Promise<boolean> => {
      if (!snapshot || snapshot.status === "idle") {
        setError("尚未载入媒体，无法跳转。");
        return false;
      }

      const upperBound = snapshot.durationSeconds > 0 ? snapshot.durationSeconds : seconds;
      const target = Math.min(Math.max(0, seconds), Math.max(0, upperBound));
      setError(null);

      if (!isDesktop) {
        setSnapshot((previous) =>
          previous
            ? {
                ...previous,
                seq: previous.seq + 1,
                positionSeconds: target,
                status: previous.status === "ended" ? "paused" : previous.status,
              }
            : previous,
        );
        return true;
      }

      try {
        await invokePlayerSeek(target, true);
        if (mountedRef.current) {
          setSnapshot((previous) =>
            previous ? { ...previous, positionSeconds: target } : previous,
          );
        }
        return true;
      } catch (reason) {
        if (mountedRef.current) setError(describeError(reason, "跳转失败"));
        return false;
      }
    },
    [isDesktop, snapshot],
  );

  const setVolume = useCallback(
    async (volume: number) => {
      const next = Math.min(100, Math.max(0, Math.round(volume)));
      setError(null);

      if (!isDesktop) {
        setSnapshot((previous) =>
          previous ? { ...previous, seq: previous.seq + 1, volume: next } : previous,
        );
        return;
      }

      try {
        await invokePlayerSetVolume(next);
        if (!mountedRef.current) return;
        setSnapshot((previous) => (previous ? { ...previous, volume: next } : previous));
      } catch (reason) {
        if (mountedRef.current) setError(describeError(reason, "调整音量失败"));
      }
    },
    [isDesktop],
  );

  const toggleMuted = useCallback(async () => {
    const nextMuted = !(snapshot?.muted ?? false);
    setError(null);

    if (!isDesktop) {
      setSnapshot((previous) =>
        previous ? { ...previous, seq: previous.seq + 1, muted: nextMuted } : previous,
      );
      return;
    }

    try {
      await invokePlayerSetMuted(nextMuted);
      if (!mountedRef.current) return;
      setSnapshot((previous) => (previous ? { ...previous, muted: nextMuted } : previous));
    } catch (reason) {
      if (mountedRef.current) {
        setError(describeError(reason, nextMuted ? "静音失败" : "取消静音失败"));
      }
    }
  }, [isDesktop, snapshot?.muted]);

  const selectTrack = useCallback(
    async (kind: PlayerTrackKind, id: number) => {
      setError(null);

      const applySelection = (previous: PlayerSnapshot | null): PlayerSnapshot | null => {
        if (!previous) return previous;
        return {
          ...previous,
          audioTracks:
            kind === "audio"
              ? previous.audioTracks.map((track) => ({ ...track, selected: track.id === id }))
              : previous.audioTracks,
          subtitleTracks:
            kind === "subtitle"
              ? previous.subtitleTracks.map((track) => ({ ...track, selected: track.id === id }))
              : previous.subtitleTracks,
        };
      };

      if (!isDesktop) {
        setSnapshot((previous) => {
          const next = applySelection(previous);
          return next ? { ...next, seq: next.seq + 1 } : next;
        });
        return;
      }

      try {
        await invokePlayerSetTrack(kind, id);
        if (!mountedRef.current) return;
        setSnapshot(applySelection);
      } catch (reason) {
        if (mountedRef.current) setError(describeError(reason, "切换轨道失败"));
      }
    },
    [isDesktop],
  );

  const toggleFullscreen = useCallback(async () => {
    setError(null);

    if (!isDesktop) {
      if (!document.fullscreenEnabled) {
        setError("当前浏览器预览环境不支持全屏。");
        return;
      }
      try {
        if (document.fullscreenElement) {
          await document.exitFullscreen();
        } else {
          await document.documentElement.requestFullscreen();
        }
        setIsFullscreen(Boolean(document.fullscreenElement));
      } catch (reason) {
        setError(describeError(reason, "切换全屏失败"));
      }
      return;
    }

    try {
      const next = await invokePlayerToggleFullscreen();
      if (mountedRef.current) setIsFullscreen(next);
    } catch (reason) {
      if (mountedRef.current) setError(describeError(reason, "切换全屏失败"));
    }
  }, [isDesktop]);

  return {
    isDesktop,
    snapshot,
    status: snapshot?.status ?? "idle",
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
  };
}