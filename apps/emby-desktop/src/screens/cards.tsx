import { useEffect, useRef, useState } from "react";
import type { ReactNode } from "react";
import type { EmbyLibraryItem } from "../emby/useEmby";
import {
  formatRemainingTicks,
  itemEpisodeLabel,
  itemKindLabel,
  itemProgress,
  itemYearLabel,
} from "../emby/format";
import { Icon, MediaArtwork, ProgressBar } from "./shared";

/** 2:3 (or 16:9) poster with Plex-style badges; used by the library and detail rows. */
export function PosterCard({
  item,
  landscape,
  onSelect,
}: {
  item: EmbyLibraryItem;
  landscape: boolean;
  onSelect: (item: EmbyLibraryItem) => void;
}) {
  const progress = itemProgress(item);
  const isSeries = item.type === "Series";
  const unplayedCount = isSeries ? item.userData.unplayedItemCount ?? 0 : 0;
  const unwatched = !isSeries && !item.userData.played && progress === 0 && item.type !== "Video";
  const subtitle = itemYearLabel(item) ?? itemKindLabel(item);

  return (
    <button
      type="button"
      className={`poster-card${landscape ? " poster-card--landscape" : ""}`}
      onClick={() => onSelect(item)}
      aria-label={`查看《${item.name}》详情`}
    >
      <span className="poster-card__frame">
        <MediaArtwork src={item.posterUrl} alt="" title={item.name} loading="lazy" />
        {unwatched && <span className="badge-unwatched" aria-hidden="true" />}
        {unplayedCount > 0 && (
          <span className="badge-count" aria-hidden="true">
            {unplayedCount > 99 ? "99+" : unplayedCount}
          </span>
        )}
        {progress > 0 && <ProgressBar value={progress} className="poster-card__progress" />}
      </span>
      <span className="poster-card__title">{item.name}</span>
      <span className="poster-card__meta">{subtitle}</span>
    </button>
  );
}

/**
 * 16:9 continue-watching card: backdrop with the amber progress bar, the series
 * name for episodes and the resume point underneath. Clicking resumes playback.
 */
export function ContinueCard({
  item,
  onPlay,
}: {
  item: EmbyLibraryItem;
  onPlay: (item: EmbyLibraryItem) => void;
}) {
  const progress = itemProgress(item);
  const episode = item.type === "Episode";
  const title = episode && item.seriesName ? item.seriesName : item.name;
  const lead = episode ? itemEpisodeLabel(item) : itemYearLabel(item);
  const remaining = formatRemainingTicks(item);
  const meta = [lead, remaining].filter(Boolean).join(" · ");
  const label = episode
    ? `继续观看《${title}》${lead ? ` ${lead}` : ""} ${item.name}`
    : `继续观看《${title}》`;

  return (
    <button type="button" className="continue-card" onClick={() => onPlay(item)} aria-label={label}>
      <span className="continue-card__frame">
        <MediaArtwork src={item.backdropUrl ?? item.posterUrl} alt="" title={title} loading="lazy" />
        <ProgressBar value={progress} className="continue-card__progress" />
      </span>
      <span className="continue-card__title">{title}</span>
      <span className="continue-card__meta">{meta || "继续观看"}</span>
    </button>
  );
}

/** Horizontal hub row with Plex-style paging arrows. */
type HubVariant = "landscape" | "poster" | "person";

export function HubRow({
  title,
  count,
  variant = "landscape",
  action,
  children,
}: {
  title: string;
  count: number;
  /** Card shape, sets the column width: 16:9 cards, 2:3 posters or people. */
  variant?: HubVariant;
  /** Optional trailing control next to the paging arrows ("查看全部"). */
  action?: ReactNode;
  children: ReactNode;
}) {
  const trackRef = useRef<HTMLDivElement>(null);
  const [edges, setEdges] = useState({ start: true, end: true });

  useEffect(() => {
    const track = trackRef.current;
    if (!track) return undefined;
    const measure = () => {
      const max = track.scrollWidth - track.clientWidth;
      setEdges({ start: track.scrollLeft <= 2, end: track.scrollLeft >= max - 2 });
    };
    measure();
    track.addEventListener("scroll", measure, { passive: true });
    const observer = new ResizeObserver(measure);
    observer.observe(track);
    return () => {
      track.removeEventListener("scroll", measure);
      observer.disconnect();
    };
  }, [count]);

  const page = (direction: 1 | -1) => {
    const track = trackRef.current;
    if (!track) return;
    track.scrollBy({ left: direction * track.clientWidth * 0.9, behavior: "smooth" });
  };

  return (
    <section className="hub" aria-label={title}>
      <div className="hub__header">
        <h2 className="hub__title">{title}</h2>
        {(action || !(edges.start && edges.end)) && (
          <div className="hub__actions">
            {action}
            {!(edges.start && edges.end) && (
              <div className="hub__nav">
                <button type="button" className="icon-button" onClick={() => page(-1)} disabled={edges.start} aria-label="向左滚动">
                  <Icon name="chevron-left" size={18} />
                </button>
                <button type="button" className="icon-button" onClick={() => page(1)} disabled={edges.end} aria-label="向右滚动">
                  <Icon name="chevron-right" size={18} />
                </button>
              </div>
            )}
          </div>
        )}
      </div>
      <div className={`hub__track hub__track--${variant}`} ref={trackRef}>
        {children}
      </div>
    </section>
  );
}
