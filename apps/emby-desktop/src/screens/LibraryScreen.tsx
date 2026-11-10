import { useEffect, useRef } from "react";
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

interface LibraryScreenProps {
  session: EmbySession | null;
  desktop: boolean;
  library: EmbyLibraryController;
  onOpen: (item: EmbyLibraryItem) => void;
}

const SKELETON_COUNT = 14;

function ContinueCard({
  item,
  onSelect,
}: {
  item: EmbyLibraryItem;
  onSelect: (item: EmbyLibraryItem) => void;
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
    <button type="button" className="continue-card" onClick={() => onSelect(item)} aria-label={label}>
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

export default function LibraryScreen({ session, desktop, library, onOpen }: LibraryScreenProps) {
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

  return (
    <main className="page">
      {library.resumable.length > 0 && (
        <HubRow title="继续观看" count={library.resumable.length}>
          {library.resumable.map((item) => (
            <ContinueCard key={item.id} item={item} onSelect={onOpen} />
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
            {library.totalItems > 0 && <span className="page-count">{library.totalItems} 项</span>}
          </div>

          {library.items.length === 0 ? (
            <p className="empty-state">{view ? `「${view.name}」中暂无内容。` : "当前媒体库暂无内容。"}</p>
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