import { useEffect, useMemo, useRef, useState } from "react";
import type { EmbySession } from "../emby/contract";
import type { EmbyLibraryController, EmbyLibraryItem } from "../emby/useEmby";
import {
  formatRemainingTicks,
  itemEpisodeLabel,
  itemProgress,
  itemYearLabel,
} from "../emby/format";
import { collectionTypeOf } from "../emby/library";
import { ErrorNotice, MediaArtwork, ProgressBar } from "./shared";
import { HubRow, PosterCard } from "./cards";
import LibraryToolbar, { DEFAULT_LIBRARY_QUERY, isDefaultLibraryQuery } from "./LibraryToolbar";
import type { LibraryQuery } from "./LibraryToolbar";

export interface LibraryScreenProps {
  session: EmbySession | null;
  desktop: boolean;
  library: EmbyLibraryController;
  onOpen: (item: EmbyLibraryItem) => void;
  /** Continue-watching cards start playback directly instead of opening details. */
  onPlay: (item: EmbyLibraryItem) => void;
  /**
   * Controlled toolbar query. Optional: when omitted LibraryScreen keeps the
   * selection in local state and sorts/filters the loaded page itself until
   * the library data layer accepts query parameters.
   */
  query?: LibraryQuery;
  /** Called whenever the toolbar selection changes. */
  onQueryChange?: (query: LibraryQuery) => void;
}

const SKELETON_COUNT = 14;

const NAME_COLLATOR = new Intl.Collator("zh-Hans-CN", {
  numeric: true,
  sensitivity: "base",
});

function compareNullableNumbers(
  a: number | null,
  b: number | null,
  direction: number,
): number {
  if (a === null && b === null) return 0;
  if (a === null) return 1;
  if (b === null) return -1;
  return (a - b) * direction;
}

/**
 * Client-side fallback while useEmby does not take sort/filter parameters yet.
 * The server will eventually return the right page; until then the loaded page
 * is ordered/filtered locally so the toolbar is not dead UI.
 */
function applyLocalLibraryQuery(
  items: EmbyLibraryItem[],
  query: LibraryQuery,
): EmbyLibraryItem[] {
  const direction = query.sortOrder === "Ascending" ? 1 : -1;
  let next = items;
  if (query.itemType !== "all") {
    next = next.filter((item) => item.type === query.itemType);
  }
  if (query.unplayedOnly) {
    next = next.filter((item) => !item.userData.played);
  }

  switch (query.sortBy) {
    case "SortName":
      return [...next].sort((a, b) => {
        const byName = NAME_COLLATOR.compare(a.name, b.name);
        if (byName !== 0) return byName * direction;
        return compareNullableNumbers(a.productionYear, b.productionYear, direction);
      });
    case "ProductionYear":
      return [...next].sort(
        (a, b) =>
          compareNullableNumbers(a.productionYear, b.productionYear, direction) ||
          NAME_COLLATOR.compare(a.name, b.name),
      );
    case "CommunityRating":
      return [...next].sort(
        (a, b) =>
          compareNullableNumbers(a.communityRating, b.communityRating, direction) ||
          NAME_COLLATOR.compare(a.name, b.name),
      );
    case "DateCreated":
      // EmbyItem does not carry DateCreated yet: keep the server order until
      // useEmby/contract expose the field and sort server-side.
      return next;
  }
}

function ContinueCard({
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

function LibrarySkeleton({ label, landscape }: { label: string; landscape: boolean }) {
  return (
    <div className="loading-state" role="status" aria-live="polite">
      <span className="visually-hidden">{label}</span>
      <div className={`poster-grid${landscape ? " poster-grid--landscape" : ""}`} aria-hidden="true">
        {Array.from({ length: SKELETON_COUNT }, (_, index) => (
          <span className="skeleton-card" key={index}>
            <span className={`skeleton-block${landscape ? " skeleton-block--landscape" : ""}`} />
            <span className="skeleton-line" />
            <span className="skeleton-line skeleton-line--short" />
          </span>
        ))}
      </div>
    </div>
  );
}

export default function LibraryScreen({
  session,
  desktop,
  library,
  onOpen,
  onPlay,
  query: controlledQuery,
  onQueryChange,
}: LibraryScreenProps) {
  const previewMode = !session || !desktop;
  const sentinelRef = useRef<HTMLDivElement>(null);
  const [localQuery, setLocalQuery] = useState<LibraryQuery>(DEFAULT_LIBRARY_QUERY);
  const { hasMore, loadMore, loadingMore, status, view } = library;
  const collection = collectionTypeOf(view);
  const landscape = collection === "homevideos";

  const query = controlledQuery ?? localQuery;
  // Movies/tv views are already restricted by includeItemTypes, so a type
  // filter only makes sense for mixed (unknown collection type) libraries.
  const availableTypes: Array<"Movie" | "Series"> =
    collection === "unknown" ? ["Movie", "Series"] : [];
  const typeFilterSupported = availableTypes.length > 1;
  const effectiveQuery: LibraryQuery =
    typeFilterSupported || query.itemType === "all"
      ? query
      : { ...query, itemType: "all" };

  // The server already returns SortName/Ascending by default, so leave that
  // order untouched; any explicit sort/filter is applied to the loaded page
  // until useEmby takes over the query server-side.
  const displayedItems = useMemo(
    () =>
      isDefaultLibraryQuery(effectiveQuery)
        ? library.items
        : applyLocalLibraryQuery(library.items, effectiveQuery),
    [library.items, effectiveQuery],
  );

  const handleQueryChange = (next: LibraryQuery) => {
    if (controlledQuery === undefined) setLocalQuery(next);
    onQueryChange?.(next);
  };

  const filtersActive = effectiveQuery.unplayedOnly || effectiveQuery.itemType !== "all";
  const localFilterCount = filtersActive && onQueryChange === undefined;

  useEffect(() => {
    const node = sentinelRef.current;
    if (!node || !hasMore) return;
    const observer = new IntersectionObserver(
      (entries) => {
        if (entries.some((entry) => entry.isIntersecting)) loadMore();
      },
      { rootMargin: "480px 0px" },
    );
    observer.observe(node);
    return () => observer.disconnect();
  }, [hasMore, loadMore, library.items.length, status, view?.id]);

  if (previewMode) {
    return (
      <main className="page">
        <div className="notice" role="status">
          预览模式：浏览器中不会连接 Emby。请在 Tauri 桌面客户端中登录后浏览媒体库。
        </div>
        <p className="empty-state">当前仅展示界面结构。登录、媒体库、详情与播放需要在 Tauri 桌面环境使用。</p>
      </main>
    );
  }

  const blockingError = status === "error" ? library.error : null;
  const appendError = status === "ready" ? library.error : null;

  return (
    <main className="page">
      {library.resumable.length > 0 && (
        <HubRow title="继续观看" count={library.resumable.length}>
          {library.resumable.map((item) => (
            <ContinueCard key={item.id} item={item} onPlay={onPlay} />
          ))}
        </HubRow>
      )}

      {status === "loading" && library.items.length === 0 ? (
        <>
          <div className="page-header">
            <h1 className="page-title">{view?.name ?? "媒体库"}</h1>
          </div>
          <LibrarySkeleton label={view ? `正在读取「${view.name}」…` : "正在读取媒体库…"} landscape={landscape} />
        </>
      ) : blockingError ? (
        <div className="page-message">
          <ErrorNotice message={blockingError} onRetry={library.reload} retryLabel="重新读取" />
        </div>
      ) : library.views.length === 0 ? (
        <p className="empty-state">服务器没有返回可用的媒体库。</p>
      ) : (
        <section aria-labelledby="library-title">
          <div className="page-header">
            <h1 className="page-title" id="library-title">
              {view?.name ?? "媒体库"}
            </h1>
            {library.totalItems > 0 && (
              <span className="page-count">
                {localFilterCount ? `${displayedItems.length} 项` : `${library.totalItems} 项`}
              </span>
            )}
          </div>

          <LibraryToolbar
            value={effectiveQuery}
            onChange={handleQueryChange}
            availableTypes={availableTypes}
          />

          {library.items.length === 0 ? (
            <p className="empty-state">{view ? `「${view.name}」中暂无内容。` : "当前媒体库暂无内容。"}</p>
          ) : displayedItems.length === 0 ? (
            <p className="empty-state">没有符合当前筛选条件的内容。</p>
          ) : (
            <div className={`poster-grid${landscape ? " poster-grid--landscape" : ""}`}>
              {displayedItems.map((item) => (
                <PosterCard key={item.id} item={item} landscape={landscape} onSelect={onOpen} />
              ))}
            </div>
          )}

          {appendError && (
            <div className="page-message">
              <ErrorNotice message={appendError} onRetry={library.reload} retryLabel="重新读取" />
            </div>
          )}

          {hasMore && (
            <div className="load-sentinel" ref={sentinelRef} aria-live="polite">
              {loadingMore ? <span className="spinner spinner--small" aria-label="正在加载更多" /> : "滚动以加载更多"}
            </div>
          )}
        </section>
      )}
    </main>
  );
}
