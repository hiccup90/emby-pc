import { useCallback, useEffect, useRef, useState } from "react";
import type { MouseEvent as ReactMouseEvent } from "react";
import { getCurrentWindow } from "@tauri-apps/api/window";
import type { Window as TauriWindow } from "@tauri-apps/api/window";
import { hasTauriRuntime } from "../emby/client";
import { BrandMark, Icon } from "./shared";

type WindowAction = "minimize" | "toggle-maximize" | "close";

/** Signed-in user shown in the title bar. `userName` is null in browser preview. */
export interface TitleBarAccount {
  userName: string | null;
  serverName: string | null;
  serverUrl: string | null;
  logoutPending: boolean;
  onLogout: () => void;
}

interface TitleBarProps {
  /** Shows a back button (detail page). */
  onBack?: () => void;
  backLabel?: string;
  account?: TitleBarAccount | null;
}

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

function AccountMenu({ account }: { account: TitleBarAccount }) {
  const [open, setOpen] = useState(false);
  const rootRef = useRef<HTMLDivElement>(null);
  const buttonRef = useRef<HTMLButtonElement>(null);

  useEffect(() => {
    if (!open) return undefined;
    const onPointerDown = (event: PointerEvent) => {
      if (!rootRef.current?.contains(event.target as Node)) setOpen(false);
    };
    // Capture phase, so the detail page's Escape (= back) does not also fire.
    const onKeyDown = (event: KeyboardEvent) => {
      if (event.key !== "Escape") return;
      event.preventDefault();
      setOpen(false);
      buttonRef.current?.focus();
    };
    window.addEventListener("pointerdown", onPointerDown, true);
    window.addEventListener("keydown", onKeyDown, true);
    return () => {
      window.removeEventListener("pointerdown", onPointerDown, true);
      window.removeEventListener("keydown", onKeyDown, true);
    };
  }, [open]);

  const name = account.userName?.trim() || null;
  const initial = (name ? Array.from(name)[0] : "?").toUpperCase();
  const server = account.serverName || account.serverUrl || "未连接服务器";

  return (
    <div className="account" ref={rootRef}>
      <button
        ref={buttonRef}
        type="button"
        className="account__button"
        aria-haspopup="menu"
        aria-expanded={open}
        aria-label={name ? `账户：${name}` : "预览模式"}
        title={name ?? "预览模式"}
        onClick={() => setOpen((value) => !value)}
      >
        <span className="avatar" aria-hidden="true">
          {initial}
        </span>
      </button>
      {open && (
        <div className="account__menu" role="menu" aria-label="账户">
          <div className="account__identity">
            <span className="avatar avatar--large" aria-hidden="true">
              {initial}
            </span>
            <span className="account__text">
              <strong>{name ?? "预览模式"}</strong>
              <span title={account.serverUrl ?? undefined}>{server}</span>
            </span>
          </div>
          <button
            type="button"
            role="menuitem"
            className="account__item"
            autoFocus
            disabled={account.logoutPending}
            onClick={() => {
              setOpen(false);
              account.onLogout();
            }}
          >
            <Icon name="logout" size={16} />
            {account.logoutPending ? "退出中…" : name ? "退出登录" : "返回登录"}
          </button>
        </div>
      )}
    </div>
  );
}

/**
 * Custom title bar for every non-player screen: back button, wordmark, account
 * menu and the window buttons. Everything that is not a control is a Tauri drag
 * region. Browser previews hide the window buttons, so no window API is touched
 * without a Tauri runtime.
 */
export default function TitleBar({ onBack, backLabel = "返回", account = null }: TitleBarProps) {
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
    if (target instanceof HTMLElement && target.closest("button, [role=menu]")) return;
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
      <div className="titlebar__lead" data-tauri-drag-region="true">
        {onBack && (
          <button
            type="button"
            className="titlebar__back"
            onClick={onBack}
            aria-label={backLabel}
            title={backLabel}
          >
            <Icon name="arrow-left" size={18} />
          </button>
        )}
        <BrandMark drag />
      </div>
      <div className="titlebar__drag" data-tauri-drag-region="true" aria-hidden="true" />
      {account && <AccountMenu account={account} />}
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
