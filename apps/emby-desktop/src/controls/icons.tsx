import type { ReactNode } from "react";

export type OverlayIconName =
  | "play"
  | "pause"
  | "volume"
  | "volume-off"
  | "captions"
  | "audio"
  | "fullscreen"
  | "back"
  | "retry"
  | "chevron-up"
  | "chevron-down";

const PATHS: Record<OverlayIconName, ReactNode> = {
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
  fullscreen: <path d="M8 4H4v4m12-4h4v4M8 20H4v-4m12 4h4v-4" />,
  back: (
    <>
      <path d="m11 18-6-6 6-6" />
      <path d="M5 12h14" />
    </>
  ),
  retry: (
    <>
      <path d="M19 12a7 7 0 1 1-2.05-4.95" />
      <path d="M19 4v5h-5" />
    </>
  ),
  "chevron-up": <path d="m7 14 5-5 5 5" />,
  "chevron-down": <path d="m7 10 5 5 5-5" />,
};

export function OverlayIcon({
  name,
  size = 17,
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
      strokeWidth={1.7}
      strokeLinecap="round"
      strokeLinejoin="round"
    >
      {PATHS[name]}
    </svg>
  );
}
