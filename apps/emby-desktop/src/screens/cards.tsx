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
  subtitle: subtitleOverride,
}: {
  item: EmbyLibraryItem;
  landscape: boolean;
  onSelect: (item: EmbyLibraryItem) => void;
  /** Overrides the default year/type line (e.g. series + episode for search hits). */
  subtitle?: string | null;
}) {
  const progress = itemProgress(item);
  const isSeries = item.type === "Series";
  const unplayedCount = isSeries ? item.userData.unplayedItemCount ?? 0 : 0;
  const unwatched = !isSeries && !item.userData.played && progress === 0 && item.type !== "Video";
  const subtitle = subtitleOverride ?? itemYearLabel(item) ?? itemKindLabel(item);

  return (
    <button
      type="button"
      className={`poster-card${landscape ? " poster-card--landscape" : ""}`}
      onClick={() => onSelect(item)}
      aria-label={`查看《${item.name}》详情`}
    >
      <span className="poster-card__frame">
        <MediaArtwork
          // Emby only publishes ImageTags.Thumb for some item types; a real
          // movie library usually has Primary but no Thumb, so thumbnail view
          // must fall back to the poster instead of showing "无图" for every
          // card.
          src={landscape ? item.thumbUrl ?? item.posterUrl : item.posterUrl}
          alt=""
          title={item.name}
          fallbackLabel="无图"
          loading="lazy"
        />
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
 * name for episodes and the resume point underneath. Clicking anywhere opens
 * the detail page; the centred play button resumes playback.
 */
export function ContinueCard({
  item,
  onPlay,
  onDetails,
}: {
  item: EmbyLibraryItem;
  onPlay: (item: EmbyLibraryItem) => void;
  onDetails: (item: EmbyLibraryItem) => void;
}) {
  const progress = itemProgress(item);
  const episode = item.type === "Episode";
  const title = episode && item.seriesName ? item.seriesName : item.name;
  const lead = episode ? itemEpisodeLabel(item) : itemYearLabel(item);
  const remaining = formatRemainingTicks(item);
  const metaMain = (episode ? [lead, item.name] : [lead]).filter(Boolean).join(" · ");
  const label = episode
    ? `继续观看《${title}》${lead ? ` ${lead}` : ""} ${item.name}`
    : `继续观看《${title}》`;

  return (
    <div className="continue-card">
      <button
        type="button"
        className="continue-card__open"
        onClick={() => onDetails(item)}
        aria-label={`查看《${title}》详情`}
      >
        <span className="continue-card__frame">
          <MediaArtwork src={item.backdropUrl ?? item.posterUrl} alt="" title={title} loading="lazy" />
          <ProgressBar value={progress} className="continue-card__progress" />
        </span>
        <span className="continue-card__title">{title}</span>
        <span className="continue-card__meta">
          <span className="continue-card__meta-main">{metaMain || "继续观看"}</span>
          {remaining && <span className="continue-card__meta-sub">{remaining}</span>}
        </span>
      </button>
      <button
        type="button"
        className="continue-card__play"
        onClick={() => onPlay(item)}
        aria-label={label}
        title="继续播放"
      >
        <Icon name="play" size={20} />
      </button>
    </div>
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

  // Vertical wheel over a hub row pans it horizontally until the row hits an
  // edge; after that the page scrolls normally again. The listener must be
  // non-passive to be able to preventDefault.
  useEffect(() => {
    const track = trackRef.current;
    if (!track) return undefined;
    const onWheel = (event: WheelEvent) => {
      const max = track.scrollWidth - track.clientWidth;
      if (max <= 0) return;
      const delta = event.deltaY !== 0 ? event.deltaY : event.deltaX;
      if (delta === 0) return;
      const atStart = track.scrollLeft <= 0;
      const atEnd = track.scrollLeft >= max - 1;
      if ((delta < 0 && atStart) || (delta > 0 && atEnd)) return;
      event.preventDefault();
      track.scrollLeft = Math.min(max, Math.max(0, track.scrollLeft + delta));
    };
    track.addEventListener("wheel", onWheel, { passive: false });
    return () => track.removeEventListener("wheel", onWheel);
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
