import { useCallback, useEffect, useRef, useState } from "react";
import type { MouseEvent as ReactMouseEvent } from "react";
import { getCurrentWindow } from "@tauri-apps/api/window";
import type { Window as TauriWindow } from "@tauri-apps/api/window";
import { hasTauriRuntime } from "../emby/client";

type WindowAction = "minimize" | "toggle-maximize" | "close";

function currentWindow(): TauriWindow | null {
  if (!hasTauriRuntime()) return null;
  try {
    return getCurrentWindow();
  } catch (reason) {
    console.warn("[titlebar] window API is unavailable", reason);
    return null;
  }
}

function MinimizeIcon() {
  return (
    <svg aria-hidden="true" width="10" height="10" viewBox="0 0 10 10">
      <path d="M1 5h8" fill="none" stroke="currentColor" strokeWidth="1" />
    </svg>
  );
}

function MaximizeIcon() {
  return (
    <svg aria-hidden="true" width="10" height="10" viewBox="0 0 10 10">
      <rect
        x="1.5"
        y="1.5"
        width="7"
        height="7"
        fill="none"
        stroke="currentColor"
        strokeWidth="1"
      />
    </svg>
  );
}

function RestoreIcon() {
  return (
    <svg aria-hidden="true" width="10" height="10" viewBox="0 0 10 10">
      <path
        d="M3.5 2.5V1.5h5v5h-1"
        fill="none"
        stroke="currentColor"
        strokeWidth="1"
      />
      <rect
        x="1.5"
        y="3.5"
        width="5"
        height="5"
        fill="none"
        stroke="currentColor"
        strokeWidth="1"
      />
    </svg>
  );
}

function CloseIcon() {
  return (
    <svg aria-hidden="true" width="10" height="10" viewBox="0 0 10 10">
      <path
        d="m1.5 1.5 7 7m0-7-7 7"
        fill="none"
        stroke="currentColor"
        strokeWidth="1"
      />
    </svg>
  );
}

/**
 * 36px custom title bar for every non-player screen. The whole bar (except the
 * window buttons) is a Tauri drag region; the buttons map to the matching
 * `@tauri-apps/api/window` commands. Browser previews keep the chrome text but
 * hide the buttons, so no window API is touched without a Tauri runtime.
 */
export default function TitleBar() {
  const [desktop] = useState(hasTauriRuntime);
  const [maximized, setMaximized] = useState(false);
  const mountedRef = useRef(true);

  const refreshMaximized = useCallback(async () => {
    const win = currentWindow();
    if (!win) return;
    try {
      const next = await win.isMaximized();
      if (mountedRef.current) setMaximized(next);
    } catch (reason) {
      console.warn("[titlebar] could not read the maximized state", reason);
    }
  }, []);

  useEffect(() => {
    if (!desktop) return undefined;
    mountedRef.current = true;
    void refreshMaximized();

    const win = currentWindow();
    if (!win) return undefined;

    let disposed = false;
    let unlisten: (() => void) | null = null;
    void win
      .onResized(() => {
        void refreshMaximized();
      })
      .then((stop) => {
        if (disposed) {
          stop();
          return;
        }
        unlisten = stop;
      })
      .catch((reason) => {
        console.warn("[titlebar] could not watch window resizes", reason);
      });

    return () => {
      disposed = true;
      mountedRef.current = false;
      unlisten?.();
    };
  }, [desktop, refreshMaximized]);

  const run = useCallback(
    async (action: WindowAction) => {
      const win = currentWindow();
      if (!win) return;
      try {
        if (action === "minimize") {
          await win.minimize();
        } else if (action === "close") {
          await win.close();
        } else {
          await win.toggleMaximize();
          await refreshMaximized();
        }
      } catch (reason) {
        console.warn(`[titlebar] window action ${action} failed`, reason);
      }
    },
    [refreshMaximized],
  );

  // Tauri's drag-region script also toggles maximize on the second mousedown of
  // a double click. This handler runs before that script (the React root sits
  // inside the document), so it stops propagation and calls `toggleMaximize()`
  // exactly once. Doing it on mousedown also keeps working when the first
  // native drag consumed the matching mouseup.
  const handleMouseDown = (event: ReactMouseEvent<HTMLElement>) => {
    if (event.detail !== 2 || event.button !== 0) return;
    const target = event.target;
    if (target instanceof HTMLElement && target.closest("button")) return;
    event.stopPropagation();
    if (!desktop) return;
    void run("toggle-maximize");
  };

  return (
    <header
      className="titlebar"
      aria-label="窗口标题栏"
      data-tauri-drag-region="true"
      onMouseDown={handleMouseDown}
    >
      <span className="titlebar__brand" data-tauri-drag-region="true">
        Emby PC
      </span>
      <div
        className="titlebar__drag"
        data-tauri-drag-region="true"
        aria-hidden="true"
      />
      {desktop && (
        <div className="titlebar__controls" role="group" aria-label="窗口控制">
          <button
            className="titlebar__button"
            data-window-action="minimize"
            type="button"
            title="最小化"
            aria-label="最小化窗口"
            onClick={() => void run("minimize")}
          >
            <MinimizeIcon />
          </button>
          <button
            className="titlebar__button"
            data-window-action="toggle-maximize"
            type="button"
            title={maximized ? "还原" : "最大化"}
            aria-label={maximized ? "还原窗口" : "最大化窗口"}
            aria-pressed={maximized}
            onClick={() => void run("toggle-maximize")}
          >
            {maximized ? <RestoreIcon /> : <MaximizeIcon />}
          </button>
          <button
            className="titlebar__button titlebar__button--close"
            data-window-action="close"
            type="button"
            title="关闭"
            aria-label="关闭窗口"
            onClick={() => void run("close")}
          >
            <CloseIcon />
          </button>
        </div>
      )}
    </header>
  );
}
