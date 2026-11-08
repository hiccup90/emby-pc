import { useEffect } from "react";
import type { RefObject } from "react";
import { invokeVideoSync } from "./tauri";

// Survives route remounts; `0` also tells the Rust host that a fresh counter
// has started (webview reload / HMR). Sequence must never move backwards.
let nextVideoSyncSeq = 0;

function takeVideoSyncSeq(): number {
  const seq = nextVideoSyncSeq;
  nextVideoSyncSeq += 1;
  return seq;
}

/**
 * Keeps the native video window aligned with the DOM video slot.
 * Only active inside Tauri; the browser preview never sends commands.
 */
export function useVideoSync(slotRef: RefObject<HTMLDivElement>, enabled: boolean): void {
  useEffect(() => {
    const element = slotRef.current;
    if (!enabled || !element) return;

    let frame = 0;
    let disposed = false;
    let dprQuery: MediaQueryList | null = null;

    const send = (visibleOverride?: boolean) => {
      const rect = element.getBoundingClientRect();
      const dpr = window.devicePixelRatio || 1;
      const visible =
        visibleOverride ??
        (!disposed && document.visibilityState !== "hidden" && rect.width > 0 && rect.height > 0);

      void invokeVideoSync({
        seq: takeVideoSyncSeq(),
        visible,
        x: Math.round(rect.left),
        y: Math.round(rect.top),
        w: Math.round(rect.width),
        h: Math.round(rect.height),
        dpr,
      }).catch(() => {
        // Geometry updates are high-frequency; player command errors are surfaced elsewhere.
      });
    };

    const flush = (visibleOverride?: boolean) => {
      if (frame !== 0) {
        window.cancelAnimationFrame(frame);
        frame = 0;
      }
      send(visibleOverride);
    };

    const schedule = () => {
      if (disposed || frame !== 0) return;
      frame = window.requestAnimationFrame(() => {
        frame = 0;
        if (!disposed) send();
      });
    };

    const onDprChange = () => {
      watchDpr();
      schedule();
    };

    const watchDpr = () => {
      dprQuery?.removeEventListener("change", onDprChange);
      const dpr = window.devicePixelRatio || 1;
      dprQuery = window.matchMedia(`(resolution: ${dpr}dppx)`);
      dprQuery.addEventListener("change", onDprChange);
    };

    function onVisibilityChange() {
      if (document.visibilityState === "hidden") {
        flush(false);
      } else {
        schedule();
      }
    }

    const observer = new ResizeObserver(schedule);
    observer.observe(element);
    window.addEventListener("resize", schedule);
    window.addEventListener("scroll", schedule, true);
    document.addEventListener("visibilitychange", onVisibilityChange);
    watchDpr();
    schedule();

    return () => {
      disposed = true;
      observer.disconnect();
      window.removeEventListener("resize", schedule);
      window.removeEventListener("scroll", schedule, true);
      document.removeEventListener("visibilitychange", onVisibilityChange);
      dprQuery?.removeEventListener("change", onDprChange);
      if (frame !== 0) window.cancelAnimationFrame(frame);

      // Hide the native video window immediately when the player page unmounts.
      const dpr = window.devicePixelRatio || 1;
      void invokeVideoSync({
        seq: takeVideoSyncSeq(),
        visible: false,
        x: 0,
        y: 0,
        w: 0,
        h: 0,
        dpr,
      }).catch(() => {
        // The page is leaving; there is no UI left to report this failure to.
      });
    };
  }, [enabled, slotRef]);
}
