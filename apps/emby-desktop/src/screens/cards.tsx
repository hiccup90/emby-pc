import { useEffect, useRef } from "react";
import type {
  KeyboardEvent as ReactKeyboardEvent,
  MouseEvent as ReactMouseEvent,
  PointerEvent as ReactPointerEvent,
  ReactNode,
} from "react";
import type { EmbyLibraryItem } from "../emby/useEmby";
import {
  formatRemainingTicks,
  itemEpisodeLabel,
  itemProgress,
  itemYearLabel,
} from "../emby/format";
import { useItemActions } from "./ItemActions";
import { Icon, MediaArtwork, ProgressBar } from "./shared";

/** Context-menu key or Shift+F10 opens the same menu as a right click. */
function isContextMenuKey(event: ReactKeyboardEvent): boolean {
  return event.key === "ContextMenu" || (event.shiftKey && event.key === "F10");
}

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
  /** Overrides the default year line (e.g. series + episode for search hits). */
  subtitle?: string | null;
}) {
  const progress = itemProgress(item);
  const isSeries = item.type === "Series";
  const unplayedCount = isSeries ? item.userData.unplayedItemCount ?? 0 : 0;
  const unwatched = !isSeries && !item.userData.played && progress === 0 && item.type !== "Video";
  // The title already shows whether it is a movie or a series; only add a
  // year when the server supplied one.
  const subtitle = subtitleOverride ?? itemYearLabel(item);
  const actions = useItemActions();
  const selecting = actions?.selection.active ?? false;
  const selected = selecting && (actions?.selection.ids.has(item.id) ?? false);

  const handleClick = () => {
    if (selecting && actions) {
      actions.selection.toggle(item);
      return;
    }
    onSelect(item);
  };

  return (
    <button
      type="button"
      className={`poster-card${landscape ? " poster-card--landscape" : ""}${selecting ? " is-selecting" : ""}${selected ? " is-selected" : ""}`}
      onClick={handleClick}
      onContextMenu={(event) => {
        if (!actions) return;
        event.preventDefault();
        actions.openMenu(item, event.clientX, event.clientY);
      }}
      onKeyDown={(event) => {
        if (!actions || !isContextMenuKey(event)) return;
        event.preventDefault();
        const rect = event.currentTarget.getBoundingClientRect();
        actions.openMenu(item, rect.left + rect.width / 2, rect.top + 8);
      }}
      aria-label={selecting ? `选择《${item.name}》` : `查看《${item.name}》详情`}
      aria-pressed={selecting ? selected : undefined}
    >
      <span className="poster-card__frame">
        <MediaArtwork
          // Match Emby's own thumbnail view: use the 16:9 Thumb when the
          // server has one, otherwise its 16:9 Backdrop. A poster is 2:3 and
          // must never be cropped into this card.
          src={landscape ? item.thumbUrl ?? item.backdropUrl : item.posterUrl}
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
        {selecting && (
          <span className="poster-card__select" aria-hidden="true">
            {selected && <Icon name="check" size={14} />}
          </span>
        )}
      </span>
      <span className="poster-card__title">{item.name}</span>
      {subtitle && <span className="poster-card__meta">{subtitle}</span>}
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
  const actions = useItemActions();
  const selecting = actions?.selection.active ?? false;
  const selected = selecting && (actions?.selection.ids.has(item.id) ?? false);

  return (
    <div
      className={`continue-card${selecting ? " is-selecting" : ""}${selected ? " is-selected" : ""}`}
      onContextMenu={(event) => {
        if (!actions) return;
        event.preventDefault();
        actions.openMenu(item, event.clientX, event.clientY);
      }}
      onClickCapture={(event) => {
        if (!selecting || !actions) return;
        event.preventDefault();
        event.stopPropagation();
        actions.selection.toggle(item);
      }}
    >
      <button
        type="button"
        className="continue-card__open"
        onClick={() => onDetails(item)}
        onKeyDown={(event) => {
          if (!actions || !isContextMenuKey(event)) return;
          event.preventDefault();
          const rect = event.currentTarget.getBoundingClientRect();
          actions.openMenu(item, rect.left + rect.width / 2, rect.top + 8);
        }}
        aria-label={selecting ? `选择《${title}》` : `查看《${title}》详情`}
        aria-pressed={selecting ? selected : undefined}
      >
        <span className="continue-card__frame">
          <MediaArtwork src={item.backdropUrl ?? item.posterUrl} alt="" title={title} loading="lazy" />
          <ProgressBar value={progress} className="continue-card__progress" />
          {selecting && (
            <span className="continue-card__select" aria-hidden="true">
              {selected && <Icon name="check" size={14} />}
            </span>
          )}
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

/** Horizontal hub row. Drag with the mouse (or use the wheel) to pan. */
type HubVariant = "landscape" | "poster" | "person";

export function HubRow({
  title,
  variant = "landscape",
  children,
}: {
  title: string;
  /** Card shape, sets the column width: 16:9 cards, 2:3 posters or people. */
  variant?: HubVariant;
  children: ReactNode;
}) {
  const trackRef = useRef<HTMLDivElement>(null);
  const dragRef = useRef<{
    pointerId: number;
    startX: number;
    startScroll: number;
    moved: boolean;
  } | null>(null);
  const suppressClickRef = useRef(false);

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
  }, []);

  const onPointerDown = (event: ReactPointerEvent<HTMLDivElement>) => {
    if (event.button !== 0) return;
    const track = event.currentTarget;
    if (track.scrollWidth <= track.clientWidth) return;
    dragRef.current = {
      pointerId: event.pointerId,
      startX: event.clientX,
      startScroll: track.scrollLeft,
      moved: false,
    };
    suppressClickRef.current = false;
  };

  const onPointerMove = (event: ReactPointerEvent<HTMLDivElement>) => {
    const drag = dragRef.current;
    if (!drag || drag.pointerId !== event.pointerId) return;
    const track = event.currentTarget;
    const delta = event.clientX - drag.startX;
    if (!drag.moved) {
      if (Math.abs(delta) < 4) return;
      drag.moved = true;
      track.classList.add("is-dragging");
      // Capture only after the drag threshold: a plain click must still reach
      // the card button underneath.
      track.setPointerCapture?.(event.pointerId);
    }
    event.preventDefault();
    track.scrollLeft = drag.startScroll - delta;
  };

  const endDrag = (event: ReactPointerEvent<HTMLDivElement>) => {
    const drag = dragRef.current;
    if (!drag || drag.pointerId !== event.pointerId) return;
    const track = event.currentTarget;
    if (track.hasPointerCapture?.(event.pointerId)) {
      track.releasePointerCapture(event.pointerId);
    }
    track.classList.remove("is-dragging");
    if (drag.moved) {
      // The click event follows pointerup; swallow exactly that one so a drag
      // does not open the card under the pointer.
      suppressClickRef.current = true;
      window.setTimeout(() => {
        suppressClickRef.current = false;
      }, 0);
    }
    dragRef.current = null;
  };

  const onClickCapture = (event: ReactMouseEvent<HTMLDivElement>) => {
    if (!suppressClickRef.current) return;
    suppressClickRef.current = false;
    event.preventDefault();
    event.stopPropagation();
  };

  return (
    <section className="hub" aria-label={title}>
      <div className="hub__header">
        <h2 className="hub__title">{title}</h2>
      </div>
      <div
        className={`hub__track hub__track--${variant}`}
        ref={trackRef}
        onPointerDown={onPointerDown}
        onPointerMove={onPointerMove}
        onPointerUp={endDrag}
        onPointerCancel={endDrag}
        onClickCapture={onClickCapture}
        onDragStart={(event) => event.preventDefault()}
      >
        {children}
      </div>
    </section>
  );
}
