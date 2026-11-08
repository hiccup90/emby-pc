import { useEffect, useState } from "react";
import type { ReactNode } from "react";

export type IconName =
  | "arrow-left"
  | "arrow-right"
  | "play"
  | "chevron"
  | "logout"
  | "refresh"
  | "alert"
  | "library";

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
    "arrow-right": (
      <>
        <path d="M5 12h14" />
        <path d="m13 6 6 6-6 6" />
      </>
    ),
    play: <path d="m8 5 11 7-11 7z" fill="currentColor" stroke="none" />,
    chevron: <path d="m7 10 5 5 5-5" />,
    logout: (
      <>
        <path d="M10 5H5v14h5" />
        <path d="m14 8 4 4-4 4" />
        <path d="M18 12H9" />
      </>
    ),
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
    library: (
      <>
        <rect x="4" y="5" width="5" height="14" />
        <rect x="10.5" y="5" width="4" height="14" />
        <path d="m16.2 6.2 3.2-.9 3.4 12.4-3.2.9z" />
      </>
    ),
  };

  return (
    <svg aria-hidden="true" width={size} height={size} viewBox="0 0 24 24" {...shared}>
      {paths[name]}
    </svg>
  );
}

export function BrandMark() {
  return (
    <span className="brand-mark" aria-hidden="true">
      <span />
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
  const [failed, setFailed] = useState(false);

  useEffect(() => {
    setFailed(false);
  }, [src]);

  const initial = Array.from(title.trim())[0] ?? "·";

  return (
    <span className={`media-artwork ${className}`.trim()}>
      <span className="media-artwork__fallback" aria-hidden="true">
        <span>{initial}</span>
      </span>
      {src && !failed && (
        <img
          src={src}
          alt={alt}
          loading={loading}
          decoding="async"
          onError={() => setFailed(true)}
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
