import { useState } from "react";
import type { ReactNode } from "react";

export type IconName =
  | "arrow-left"
  | "play"
  | "refresh"
  | "alert"
  | "film"
  | "tv"
  | "camera"
  | "music"
  | "folder"
  | "server"
  | "chevron-left"
  | "chevron-right"
  | "check"
  | "logout";

export function Icon({ name, size = 18 }: { name: IconName; size?: number }) {
  const shared = {
    fill: "none",
    stroke: "currentColor",
    strokeWidth: 1.7,
    strokeLinecap: "round" as const,
    strokeLinejoin: "round" as const,
  };
  const paths: Record<IconName, ReactNode> = {
    "arrow-left": (
      <>
        <path d="m11 18-6-6 6-6" />
        <path d="M5 12h14" />
      </>
    ),
    play: <path d="M8 5.2v13.6a.8.8 0 0 0 1.2.7l10.4-6.8a.8.8 0 0 0 0-1.4L9.2 4.5a.8.8 0 0 0-1.2.7z" fill="currentColor" stroke="none" />,
    refresh: (
      <>
        <path d="M20 7v5h-5" />
        <path d="M4 17v-5h5" />
        <path d="M6.1 9a7 7 0 0 1 11.5-2L20 12M4 12l2.4 5a7 7 0 0 0 11.5-2" />
      </>
    ),
    alert: (
      <>
        <path d="M12 4 3 20h18z" />
        <path d="M12 9v5" />
        <path d="M12 17.2v.1" />
      </>
    ),
    film: (
      <>
        <rect x="3.5" y="4" width="17" height="16" rx="2" />
        <path d="M7.5 4v16M16.5 4v16M3.5 9h4M3.5 15h4M16.5 9h4M16.5 15h4" />
      </>
    ),
    tv: (
      <>
        <rect x="3" y="7" width="18" height="12.5" rx="2" />
        <path d="m8.5 3 3.5 4 3.5-4" />
      </>
    ),
    camera: (
      <>
        <rect x="3" y="6.5" width="13" height="11" rx="2" />
        <path d="m16 10.5 5-3v9l-5-3" />
      </>
    ),
    music: (
      <>
        <path d="M9 17.5V6l11-2v11.5" />
        <circle cx="6.5" cy="17.5" r="2.5" />
        <circle cx="17.5" cy="15.5" r="2.5" />
      </>
    ),
    folder: <path d="M3.5 7.5a2 2 0 0 1 2-2h4l2 2h7a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2h-13a2 2 0 0 1-2-2z" />,
    server: (
      <>
        <rect x="4" y="4" width="16" height="7" rx="1.6" />
        <rect x="4" y="13" width="16" height="7" rx="1.6" />
        <path d="M8 7.5h.01M8 16.5h.01" />
      </>
    ),
    "chevron-left": <path d="m14.5 18-6-6 6-6" />,
    "chevron-right": <path d="m9.5 18 6-6-6-6" />,
    check: <path d="m5 12.5 4.5 4.5L19 7.5" />,
    logout: (
      <>
        <path d="M14.5 4h3.5a2 2 0 0 1 2 2v12a2 2 0 0 1-2 2h-3.5" />
        <path d="m9.5 16.5-4.5-4.5 4.5-4.5" />
        <path d="M5 12h11" />
      </>
    ),
  };

  return (
    <svg aria-hidden="true" width={size} height={size} viewBox="0 0 24 24" {...shared}>
      {paths[name]}
    </svg>
  );
}

/** App wordmark. `drag` keeps it part of the title bar drag region. */
export function BrandMark({ drag = false, large = false }: { drag?: boolean; large?: boolean }) {
  const dragProps = drag ? { "data-tauri-drag-region": "true" } : {};
  return (
    <span className={`brand-mark${large ? " brand-mark--large" : ""}`} {...dragProps}>
      Emby
      <span className="brand-mark__accent" {...dragProps}>
        PC
      </span>
    </span>
  );
}

interface MediaArtworkProps {
  src: string | null | undefined;
  alt: string;
  title: string;
  className?: string;
  loading?: "lazy" | "eager";
}

/**
 * Real Emby image with a deterministic local fallback. A failed image request
 * must never leave an empty frame behind.
 */
export function MediaArtwork({
  src,
  alt,
  title,
  className = "",
  loading = "lazy",
}: MediaArtworkProps) {
  // Keyed by URL instead of reset in an effect: a fast (cached) load must not
  // be undone by a reset that runs after it.
  const [loadedSrc, setLoadedSrc] = useState<string | null>(null);
  const [failedSrc, setFailedSrc] = useState<string | null>(null);
  const usable = Boolean(src) && failedSrc !== src;
  const loaded = usable && loadedSrc === src;

  return (
    <span className={`media-artwork ${className}`.trim()} data-loaded={loaded ? "true" : "false"}>
      {!usable && (
        <span className="media-artwork__fallback" aria-hidden="true">
          <span>{title.trim() || "·"}</span>
        </span>
      )}
      {src && usable && (
        <img
          src={src}
          alt={alt}
          loading={loading}
          decoding="async"
          onLoad={() => setLoadedSrc(src)}
          onError={() => setFailedSrc(src)}
        />
      )}
    </span>
  );
}

interface ErrorNoticeProps {
  message: string;
  onRetry?: () => void;
  retryLabel?: string;
}

export function ErrorNotice({
  message,
  onRetry,
  retryLabel = "重试",
}: ErrorNoticeProps) {
  return (
    <div className="error-notice" role="alert">
      <span className="error-notice__icon" aria-hidden="true">
        <Icon name="alert" size={17} />
      </span>
      <span className="error-notice__message">{message}</span>
      {onRetry && (
        <button className="error-notice__retry" type="button" onClick={onRetry}>
          <Icon name="refresh" size={13} />
          {retryLabel}
        </button>
      )}
    </div>
  );
}

/**
 * Faded artwork behind the content column (detail pages). Rendered under the
 * scroll container, so it stays put while the page scrolls and also shows
 * through the scrollbar track. Hidden when the image fails.
 */
export function PageBackdrop({ src }: { src: string }) {
  const [failed, setFailed] = useState(false);
  if (failed) return null;
  return (
    <div className="page-backdrop" aria-hidden="true">
      <img src={src} alt="" decoding="async" onError={() => setFailed(true)} />
    </div>
  );
}

/** Thin amber watch-progress bar (0..1). */
export function ProgressBar({ value, className = "" }: { value: number; className?: string }) {
  const percent = Math.round(Math.min(1, Math.max(0, value)) * 100);
  return (
    <span className={`progress-bar ${className}`.trim()} aria-hidden="true">
      <span style={{ width: `${percent}%` }} />
    </span>
  );
}
