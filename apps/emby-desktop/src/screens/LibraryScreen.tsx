import { useEffect, useRef } from "react";
import type { EmbySession } from "../emby/contract";
import type {
  EmbyLibraryController,
  EmbyLibraryItem,
  EmbyLibraryQuery,
} from "../emby/useEmby";
import { collectionTypeOf } from "../emby/library";
import { ErrorNotice } from "./shared";
import { ContinueCard, HubRow, PosterCard } from "./cards";
import LibraryToolbar from "./LibraryToolbar";

export interface LibraryScreenProps {
  session: EmbySession | null;
  desktop: boolean;
  library: EmbyLibraryController;
  onOpen: (item: EmbyLibraryItem) => void;
  /** Continue-watching cards start playback directly instead of opening details. */
  onPlay: (item: EmbyLibraryItem) => void;
  /** Toolbar value; the query itself lives in the library data layer. */
  query: EmbyLibraryQuery;
  onQueryChange: (query: EmbyLibraryQuery) => void;
}

const SKELETON_COUNT = 14;

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
  query,
  onQueryChange,
}: LibraryScreenProps) {
  const previewMode = !session || !desktop;
  const sentinelRef = useRef<HTMLDivElement>(null);
  const { hasMore, loadMore, loadingMore, status, view } = library;
  const landscape = collectionTypeOf(view) === "homevideos";

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
  /** First paint: no view is known yet, so the whole page is a skeleton. */
  const booting = status === "loading" && library.views.length === 0;
  /** A new sort/filter is being fetched; the toolbar and title stay usable. */
  const refetching = status === "loading" && library.views.length > 0;
  /** "Nothing here" and "nothing matched" are different messages. */
  const filtering = query.itemType !== "all" || query.unplayedOnly;
  const emptyLabel = filtering
    ? "没有符合当前筛选条件的内容。"
    : view
      ? `「${view.name}」中暂无内容。`
      : "当前媒体库暂无内容。";

  return (
    <main className="page">
      {library.resumable.length > 0 && (
        <HubRow title="继续观看" count={library.resumable.length}>
          {library.resumable.map((item) => (
            <ContinueCard key={item.id} item={item} onPlay={onPlay} />
          ))}
        </HubRow>
      )}

      {booting ? (
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
              <span className="page-count">{library.totalItems} 项</span>
            )}
          </div>

          <LibraryToolbar
            value={query}
            onChange={onQueryChange}
            showTypeFilter={library.supportsTypeFilter}
            busy={refetching}
          />

          {refetching && library.items.length === 0 ? (
            <LibrarySkeleton label="正在按新的排序与筛选重新读取…" landscape={landscape} />
          ) : library.items.length === 0 ? (
            <p className="empty-state">{emptyLabel}</p>
          ) : (
            <div className={`poster-grid${landscape ? " poster-grid--landscape" : ""}`}>
              {library.items.map((item) => (
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
