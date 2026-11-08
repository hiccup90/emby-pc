import { useEffect, useRef } from "react";
import type { EmbyPlaybackTarget, EmbySession } from "../emby/contract";
import type { EmbyLibraryItem } from "../emby/useEmby";
import { useEmbyDetail, usePlaybackLauncher } from "../emby/useEmby";
import {
  compactUrl,
  formatRuntimeTicks,
  itemEpisodeLabel,
  itemKindLabel,
  itemProgress,
  itemYearLabel,
  userInitial,
} from "../emby/format";
import {
  BrandMark,
  ErrorNotice,
  Icon,
  MediaArtwork,
} from "./shared";

interface DetailScreenProps {
  itemId: string;
  seed: EmbyLibraryItem | null;
  libraryItems: EmbyLibraryItem[];
  session: EmbySession | null;
  desktop: boolean;
  logoutPending: boolean;
  onBack: () => void;
  onOpenRelated: (item: EmbyLibraryItem) => void;
  onPlay: (target: EmbyPlaybackTarget, title: string) => void;
  onLogout: () => void;
}

function DetailLoading() {
  return (
    <div className="detail-status" role="status" aria-live="polite">
      <span className="visually-hidden">正在加载条目详情…</span>
      <div className="detail-skeleton" aria-hidden="true">
        <span className="skeleton-block skeleton-line skeleton-line--eyebrow" />
        <span className="skeleton-block skeleton-line skeleton-line--detail-title" />
        <span className="skeleton-block skeleton-line skeleton-line--meta" />
        <span className="skeleton-block skeleton-line" />
        <span className="skeleton-block skeleton-line skeleton-line--short" />
      </div>
    </div>
  );
}

export default function DetailScreen({
  itemId,
  seed,
  libraryItems,
  session,
  desktop,
  logoutPending,
  onBack,
  onOpenRelated,
  onPlay,
  onLogout,
}: DetailScreenProps) {
  const detail = useEmbyDetail(itemId, seed, desktop && Boolean(session));
  const playback = usePlaybackLauncher();
  const headingRef = useRef<HTMLHeadingElement>(null);
  const item = detail.item;
  const progress = item ? itemProgress(item) : 0;

  useEffect(() => {
    if (item) headingRef.current?.focus({ preventScroll: true });
  }, [item]);

  useEffect(() => {
    const onKeyDown = (event: KeyboardEvent) => {
      if (event.key === "Escape" && !event.defaultPrevented) {
        event.preventDefault();
        onBack();
      }
    };
    window.addEventListener("keydown", onKeyDown);
    return () => window.removeEventListener("keydown", onKeyDown);
  }, [onBack]);

  const startPlayback = async () => {
    if (!item || playback.pending) return;
    const target = await playback.launch(item.id);
    if (target) onPlay(target, item.name);
  };

  const related = libraryItems
    .filter((candidate) => candidate.id !== item?.id)
    .slice(0, 4);

  return (
    <>
      <header className="topbar topbar--detail">
        <button
          className="icon-button detail-back"
          type="button"
          onClick={onBack}
          aria-label="返回影库"
        >
          <Icon name="arrow-left" size={19} />
        </button>
        <span className="brand" aria-label="Emby 影库">
          <BrandMark />
          <span>
            EMBER
            <span className="brand__sub">EMBY DESKTOP</span>
          </span>
        </span>
        <div className="topbar__right">
          <span className="server-status">
            <i />
            <span>{session?.serverName || compactUrl(session?.serverUrl) || "Emby"}</span>
          </span>
          <span
            className="profile-mark"
            aria-label={session ? `当前用户：${session.userName}` : "当前用户"}
          >
            {session ? userInitial(session.userName) : "—"}
          </span>
          <button
            className="logout-button"
            type="button"
            onClick={onLogout}
            disabled={logoutPending}
            aria-label="退出 Emby 登录"
          >
            <Icon name="logout" size={14} />
            <span>{logoutPending ? "退出中…" : "退出"}</span>
          </button>
        </div>
      </header>

      <main className="detail-main">
        {!item && detail.loading && <DetailLoading />}

        {!item && !detail.loading && detail.error && (
          <section className="detail-status" aria-labelledby="detail-error-title">
            <span className="eyebrow">DETAIL UNAVAILABLE</span>
            <h1 id="detail-error-title">无法读取条目详情</h1>
            <ErrorNotice
              message={detail.error}
              onRetry={detail.reload}
              retryLabel="重新读取"
            />
            <button className="button button--quiet" type="button" onClick={onBack}>
              <Icon name="arrow-left" size={15} />
              返回影库
            </button>
          </section>
        )}

        {!item && !detail.loading && !detail.error && (
          <section className="detail-status" aria-labelledby="detail-missing-title">
            <span className="eyebrow">MISSING ITEM</span>
            <h1 id="detail-missing-title">条目不存在</h1>
            <p>服务器没有返回该条目的信息，可能已被移出媒体库。</p>
            <button className="button button--quiet" type="button" onClick={onBack}>
              <Icon name="arrow-left" size={15} />
              返回影库
            </button>
          </section>
        )}

        {item && (
          <>
            {detail.error && (
              <div className="detail-warning" role="alert">
                <span>{detail.error} 当前显示影库中的条目信息。</span>
                <button type="button" onClick={detail.reload}>
                  重新读取
                </button>
              </div>
            )}

            <section
              className="detail-hero"
              aria-labelledby="detail-title"
              aria-busy={detail.loading}
            >
              <MediaArtwork
                src={item.backdropUrl ?? item.posterUrl}
                alt={`《${item.name}》背景图`}
                title={item.name}
                className="detail-hero__art"
                loading="eager"
              />
              <div className="detail-hero__shade" />
              <div className="detail-hero__content">
                <div className="breadcrumb">
                  <button type="button" onClick={onBack}>
                    影库
                  </button>
                  <span>/</span>
                  {itemKindLabel(item)}
                </div>
                <p className="detail-subtitle">
                  {item.seriesName ?? itemEpisodeLabel(item) ?? "EMBY LIBRARY"}
                </p>
                <h1 id="detail-title" ref={headingRef} tabIndex={-1}>
                  {item.name}
                </h1>
                <div className="detail-meta">
                  {itemYearLabel(item) && (
                    <>
                      <span>{itemYearLabel(item)}</span>
                      <i />
                    </>
                  )}
                  <span>{itemKindLabel(item)}</span>
                  <i />
                  <span>{formatRuntimeTicks(item.runTimeTicks)}</span>
                  {progress > 0 && (
                    <>
                      <i />
                      <span>已观看 {Math.round(progress * 100)}%</span>
                    </>
                  )}
                  {item.userData.played && (
                    <>
                      <i />
                      <span>已看完</span>
                    </>
                  )}
                </div>
                <p className="detail-synopsis">
                  {item.overview?.trim() || "暂无简介。"}
                </p>
                <div className="detail-tags">
                  {item.genres.map((genre) => (
                    <span key={genre}>{genre}</span>
                  ))}
                  <span className="detail-note">
                    {detail.loading ? "正在刷新详情…" : "EMBY LIBRARY"}
                  </span>
                </div>

                <div className="detail-actions">
                  <button
                    className="button button--primary"
                    type="button"
                    onClick={() => void startPlayback()}
                    disabled={playback.pending}
                  >
                    <Icon name="play" size={16} />
                    {playback.pending
                      ? "正在准备…"
                      : progress > 0
                        ? "继续播放"
                        : "播放"}
                  </button>
                  {progress > 0 && (
                    <span className="detail-progress-label">
                      已观看 {Math.round(progress * 100)}%
                    </span>
                  )}
                </div>

                {playback.error && (
                  <div className="detail-play-error" role="alert">
                    <span>{playback.error}</span>
                    <button
                      type="button"
                      onClick={playback.clearError}
                      aria-label="关闭播放错误提示"
                    >
                      ×
                    </button>
                  </div>
                )}
              </div>
              <div className="detail-hero__side-note">
                <span>
                  EMBY LIBRARY / {itemKindLabel(item).toUpperCase()}
                  {item.indexNumber !== null
                    ? ` ${String(item.indexNumber).padStart(2, "0")}`
                    : ""}
                </span>
                <i />
              </div>
            </section>

            <section className="detail-lower" aria-label="更多影片">
              <div className="detail-lower__intro">
                <span className="eyebrow">MORE TO DISCOVER</span>
                <h2>
                  影库中的
                  <br />
                  其他故事。
                </h2>
                <button className="text-button" type="button" onClick={onBack}>
                  返回影库 <Icon name="arrow-right" size={15} />
                </button>
              </div>
              {related.length > 0 ? (
                <div className="media-rail media-rail--detail">
                  {related.map((candidate) => (
                    <RelatedCard
                      key={candidate.id}
                      item={candidate}
                      onSelect={() => onOpenRelated(candidate)}
                    />
                  ))}
                </div>
              ) : (
                <p className="detail-lower__empty">影库中暂无其他条目。</p>
              )}
            </section>
          </>
        )}
      </main>
    </>
  );
}

function RelatedCard({
  item,
  onSelect,
}: {
  item: EmbyLibraryItem;
  onSelect: () => void;
}) {
  const year = itemYearLabel(item);
  return (
    <article className="media-card">
      <button
        className="media-card__poster-button"
        type="button"
        onClick={onSelect}
        aria-label={`查看《${item.name}》详情`}
      >
        <span className="poster-frame">
          <MediaArtwork
            src={item.posterUrl}
            alt={`《${item.name}》海报`}
            title={item.name}
            loading="lazy"
          />
          <span className="poster-open" aria-hidden="true">
            <Icon name="arrow-right" size={16} />
          </span>
        </span>
      </button>
      <button
        className="media-card__info"
        type="button"
        onClick={onSelect}
        aria-label={`查看《${item.name}》详情`}
      >
        <span className="media-card__title">{item.name}</span>
        <span className="media-card__meta">
          {itemKindLabel(item)}
          {year && (
            <>
              {" "}
              <i>·</i> {year}
            </>
          )}
        </span>
      </button>
    </article>
  );
}
