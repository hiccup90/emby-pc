import type { ReactNode } from "react";

export type OverlayIconName =
  | "play"
  | "pause"
  | "replay-10"
  | "forward-30"
  | "volume"
  | "volume-low"
  | "volume-off"
  | "audio"
  | "subtitles"
  | "fullscreen"
  | "back"
  | "retry"
  | "check"
  | "close"
  | "chapters";

const SPEAKER = <path d="M4 9.5h3l4.5-4v13l-4.5-4H4z" fill="currentColor" />;

function SkipLabel({ children }: { children: string }) {
  return (
    <text
      x="12"
      y="14.9"
      textAnchor="middle"
      fontSize="7.4"
      fontWeight="700"
      fill="currentColor"
      stroke="none"
    >
      {children}
    </text>
  );
}

// 24x24 line icons drawn for this app (no third-party artwork).
const PATHS: Record<OverlayIconName, ReactNode> = {
  play: (
    <path
      d="M8 5.2v13.6a.8.8 0 0 0 1.2.7l10.6-6.8a.8.8 0 0 0 0-1.4L9.2 4.5A.8.8 0 0 0 8 5.2z"
      fill="currentColor"
      stroke="none"
    />
  ),
  pause: (
    <>
      <rect x="6.5" y="5" width="4" height="14" rx="1" fill="currentColor" stroke="none" />
      <rect x="13.5" y="5" width="4" height="14" rx="1" fill="currentColor" stroke="none" />
    </>
  ),
  // Open circle turning counter-clockwise, arrow at the top.
  "replay-10": (
    <>
      <path d="M5.45 7.4A8 8 0 1 0 12 4" />
      <path d="M13.3 1.6 10.9 4l2.4 2.4" />
      <SkipLabel>10</SkipLabel>
    </>
  ),
  "forward-30": (
    <>
      <path d="M18.55 7.4A8 8 0 1 1 12 4" />
      <path d="M10.7 1.6 13.1 4l-2.4 2.4" />
      <SkipLabel>30</SkipLabel>
    </>
  ),
  volume: (
    <>
      {SPEAKER}
      <path d="M15.5 9a4.2 4.2 0 0 1 0 6" />
      <path d="M18.3 6.3a8 8 0 0 1 0 11.4" />
    </>
  ),
  "volume-low": (
    <>
      {SPEAKER}
      <path d="M15.5 9a4.2 4.2 0 0 1 0 6" />
    </>
  ),
  "volume-off": (
    <>
      {SPEAKER}
      <path d="m15.5 9.5 5 5m0-5-5 5" />
    </>
  ),
  // Audio track: a waveform, so it does not look like the volume button.
  audio: <path d="M4 10v4M8 7v10M12 4v16M16 8v8M20 10.5v3" />,
  subtitles: (
    <>
      <rect x="3" y="5" width="18" height="14" rx="2.5" />
      <path d="M7 11.5h4m2 0h4M7 15h7m2 0h1" />
    </>
  ),
  fullscreen: (
    <path d="M4 9V5.5A1.5 1.5 0 0 1 5.5 4H9m6 0h3.5A1.5 1.5 0 0 1 20 5.5V9m0 6v3.5a1.5 1.5 0 0 1-1.5 1.5H15m-6 0H5.5A1.5 1.5 0 0 1 4 18.5V15" />
  ),
  back: <path d="M15 5 8 12l7 7" />,
  retry: (
    <>
      <path d="M20 12a8 8 0 1 1-2.34-5.66" />
      <path d="M20 4v4.5h-4.5" />
    </>
  ),
  check: <path d="m5 12.5 4.5 4.5L19 7.5" />,
  close: <path d="M6.5 6.5l11 11m0-11-11 11" />,
  chapters: (
    <>
      <path d="M9 7h11M9 12h11M9 17h11" />
      <circle cx="5" cy="7" r="1.3" fill="currentColor" stroke="none" />
      <circle cx="5" cy="12" r="1.3" fill="currentColor" stroke="none" />
      <circle cx="5" cy="17" r="1.3" fill="currentColor" stroke="none" />
    </>
  ),
};

export function OverlayIcon({
  name,
  size = 20,
}: {
  name: OverlayIconName;
  size?: number;
}) {
  return (
    <svg
      aria-hidden="true"
      width={size}
      height={size}
      viewBox="0 0 24 24"
      fill="none"
      stroke="currentColor"
      strokeWidth={1.8}
      strokeLinecap="round"
      strokeLinejoin="round"
    >
      {PATHS[name]}
    </svg>
  );
}
