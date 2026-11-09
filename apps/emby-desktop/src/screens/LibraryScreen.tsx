import { useEffect, useRef } from "react";
import type { EmbySession } from "../emby/contract";
import type {
  EmbyLibraryController,
  EmbyLibraryItem,
} from "../emby/useEmby";
import {
  formatRemainingTicks,
  itemKindLabel,
  itemProgress,
  itemYearLabel,
} from "../emby/format";
import { ErrorNotice, MediaArtwork } from "./shared";

interface LibraryScreenProps {
  session: EmbySession | null;
  desktop: boolean;
  library: EmbyLibraryController;
  logoutPending: boolean;
  onOpen: (item: EmbyLibraryItem) => void;
  onLogout: () => void;
}

const SKELETON_COUNT = 12;

function MediaCard({
  item,
  onSelect,
}: {
  item: EmbyLibraryItem;
  onSelect: (item: EmbyLibraryItem) => void;
}) {
  const year = itemYearLabel(item);
  const meta = [itemKindLabel(item), year].filter(Boolean).join(" · ");

  return (
    <article className="media-card">
      <button
        className="media-card__poster"
        type="button"
        onClick={() => onSelect(item)}
        aria-label={`查看《${item.name}》详情`}
      >
        <MediaArtwork
          src={item.posterUrl}
          alt={`《${item.name}》海报`}
          title={item.name}
          loading="lazy"
        />
      </button>
      <button
        className="media-card__info"
        type="button"
        onClick={() => onSelect(item)}
        aria-label={`查看《${item.name}》详情`}
      >
        <span className="media-card__title">{item.name}</span>
        <span className="media-card__meta">{meta}</span>
      </button>
    </article>
  );
}

function ContinueCard({
  item,
  onSelect,
}: {
  item: EmbyLibraryItem;
  onSelect: (item: EmbyLibraryItem) => void;
}) {
  const progress = itemProgress(item);
  const percent = Math.round(progress * 100);
  const remaining = formatRemainingTicks(item) ?? "继续观看";
  const title =
    item.type === "Episode" && item.seriesName
      ? `${item.seriesName} · ${item.name}`
      : item.name;

  return (
    <button
      className="continue-card"
      type="button"
      onClick={() => onSelect(item)}
      aria-label={`继续观看《${title}》，已观看 ${percent}%`}
    >
      <span className="continue-card__art">
        <MediaArtwork
          src={item.backdropUrl ?? item.posterUrl}
          alt={`《${title}》画面`}
          title={item.name}
          loading="lazy"
        />
      </span>
      <span className="continue-card__body">
        <span className="continue-card__title">{title}</span>
        <span className="continue-card__remaining">{remaining}</span>
      </span>
      <span className="continue-card__progress" aria-hidden="true">
        <span style={{ width: `${percent}%` }} />
      </span>
    </button>
  );
}

function LibrarySkeleton({ label }: { label: string }) {
  return (
    <div className="loading-state" role="status" aria-live="polite">
      <span className="loading-line">{label}</span>
      <div className="skeleton-grid" aria-hidden="true">
        {Array.from({ length: SKELETON_COUNT }, (_, index) => (
          <span className="skeleton-card" key={index}>
            <span className="skeleton-poster" />
            <span className="skeleton-text" />
            <span className="skeleton-text skeleton-text--short" />
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
  logoutPending,
  onOpen,
  onLogout,
}: LibraryScreenProps) {
  const previewMode = !session || !desktop;
  const sentinelRef = useRef<HTMLDivElement>(null);
  const { hasMore, loadMore, loadingMore, status, view } = library;

  useEffect(() => {
    const node = sentinelRef.current;
    if (!node || !hasMore) return;
    const observer = new IntersectionObserver(
      (entries) => {
        if (entries.some((entry) => entry.isIntersecting)) loadMore();
      },
      { rootMargin: "320px 0px" },
    );
    observer.observe(node);
    return () => observer.disconnect();
  }, [hasMore, loadMore, library.items.length, status, view?.id]);

  const tabStatus =
    library.views.length > 0
      ? null
      : previewMode
        ? "预览模式"
        : status === "error"
          ? "媒体库读取失败"
          : status === "ready"
            ? "没有可用的媒体库"
            : "正在读取媒体库…";

  const blockingError = status === "error" ? library.error : null;
  const appendError = status === "ready" ? library.error : null;

  return (
    <>
      <header className="topbar">
        <div className="topbar__brand">Emby PC</div>
        <nav className="view-tabs" aria-label="媒体库">
          {library.views.map((entry) => (
            <button
              key={entry.id}
              className={`view-tab${
                library.view?.id === entry.id ? " is-active" : ""
              }`}
              type="button"
              aria-current={library.view?.id === entry.id ? "page" : undefined}
              onClick={() => library.selectView(entry.id)}
            >
              {entry.name}
            </button>
          ))}
          {tabStatus && <span className="view-tabs__status">{tabStatus}</span>}
        </nav>
        <div className="topbar__right">
          <span
            className="topbar__user"
            title={session?.serverName || session?.serverUrl}
          >
            {session ? session.userName : "预览模式"}
          </span>
          <button
            className="text-button"
            type="button"
            onClick={onLogout}
            disabled={logoutPending}
            aria-label={previewMode ? "返回登录页" : "退出 Emby 登录"}
          >
            {logoutPending ? "退出中…" : previewMode ? "返回登录" : "退出"}
          </button>
        </div>
      </header>

      <main className="content">
        {previewMode ? (
          <>
            <div className="preview-callout" role="status">
              预览模式：浏览器中不会连接 Emby。请在 Tauri 桌面客户端中登录后浏览媒体库。
            </div>
            <p className="empty-state">
              当前仅展示界面结构。登录、媒体库、详情与播放需要在 Tauri 桌面环境使用。
            </p>
          </>
        ) : (
          <>
            {library.resumable.length > 0 && (
              <section className="section" aria-labelledby="continue-title">
                <div className="section-header">
                  <h2 className="section-title" id="continue-title">
                    继续观看
                  </h2>
                  <span className="section-meta">
                    {library.resumable.length} 个
                  </span>
                </div>
                <div className="continue-grid">
                  {library.resumable.map((item) => (
                    <ContinueCard key={item.id} item={item} onSelect={onOpen} />
                  ))}
                </div>
              </section>
            )}

            {status === "loading" && library.items.length === 0 ? (
              <LibrarySkeleton
                label={view ? `正在读取「${view.name}」…` : "正在读取媒体库…"}
              />
            ) : blockingError ? (
              <ErrorNotice
                message={blockingError}
                onRetry={library.reload}
                retryLabel="重新读取"
              />
            ) : library.views.length === 0 ? (
              <p className="empty-state">服务器没有返回可用的媒体库。</p>
            ) : (
              <section className="section" aria-labelledby="library-title">
                <div className="section-header">
                  <h2 className="section-title" id="library-title">
                    {view?.name ?? "媒体库"}
                  </h2>
                  <span className="section-meta">
                    {library.totalItems > 0 ? `${library.totalItems} 项` : ""}
                  </span>
                </div>

                {library.items.length === 0 ? (
                  <p className="empty-state">
                    {view ? `「${view.name}」中暂无内容。` : "当前媒体库暂无内容。"}
                  </p>
                ) : (
                  <div className="poster-grid">
                    {library.items.map((item) => (
                      <MediaCard key={item.id} item={item} onSelect={onOpen} />
                    ))}
                  </div>
                )}

                {appendError && (
                  <div className="load-error">
                    <ErrorNotice
                      message={appendError}
                      onRetry={library.reload}
                      retryLabel="重新读取"
                    />
                  </div>
                )}

                {hasMore && (
                  <div
                    className="load-sentinel"
                    ref={sentinelRef}
                    aria-live="polite"
                  >
                    {loadingMore ? "正在加载更多…" : "滚动以加载更多"}
                  </div>
                )}
              </section>
            )}
          </>
        )}
      </main>
    </>
  );
}
