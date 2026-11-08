import { useEffect, useRef } from "react";
import type { EmbySession } from "../emby/contract";
import type { EmbyLibraryItem, EmbyLibraryState } from "../emby/useEmby";
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

interface LibraryScreenProps {
  session: EmbySession | null;
  desktop: boolean;
  library: EmbyLibraryState & { reload: () => void };
  logoutPending: boolean;
  onOpen: (item: EmbyLibraryItem) => void;
  onLogout: () => void;
}

function MediaCard({
  item,
  progress = false,
  onSelect,
}: {
  item: EmbyLibraryItem;
  progress?: boolean;
  onSelect: (item: EmbyLibraryItem) => void;
}) {
  const percent = Math.round(itemProgress(item) * 100);
  const year = itemYearLabel(item);
  const kind = itemKindLabel(item);

  return (
    <article className={`media-card${progress ? " media-card--progress" : ""}`}>
      <button
        className="media-card__poster-button"
        type="button"
        onClick={() => onSelect(item)}
        aria-label={`查看《${item.name}》详情${progress ? `，已观看 ${percent}%` : ""}`}
      >
        <span className="poster-frame">
          <MediaArtwork
            src={item.posterUrl}
            alt={`《${item.name}》海报`}
            title={item.name}
            loading="lazy"
          />
          {progress && percent > 0 && (
            <span className="poster-progress" aria-hidden="true">
              <span style={{ width: `${percent}%` }} />
            </span>
          )}
          <span className="poster-open" aria-hidden="true">
            <Icon name="arrow-right" size={16} />
          </span>
        </span>
        {progress && (
          <span className="continue-action" aria-hidden="true">
            继续观看 <Icon name="arrow-right" size={13} />
          </span>
        )}
      </button>
      <button
        className="media-card__info"
        type="button"
        onClick={() => onSelect(item)}
        aria-label={`查看《${item.name}》详情`}
      >
        <span className="media-card__title">{item.name}</span>
        <span className="media-card__meta">
          {kind}
          {year && (
            <>
              {" "}
              <i>·</i> {year}
            </>
          )}
          {progress && percent > 0 && (
            <>
              {" "}
              <i>·</i> {percent}%
            </>
          )}
        </span>
      </button>
    </article>
  );
}

function LibrarySkeleton() {
  return (
    <div className="library-loading" role="status" aria-live="polite">
      <span className="visually-hidden">正在加载 Emby 媒体库…</span>
      <div className="feature feature--skeleton" aria-hidden="true">
        <div className="skeleton-block feature__skeleton-art" />
        <div className="feature__content feature__content--skeleton">
          <span className="skeleton-block skeleton-line skeleton-line--eyebrow" />
          <span className="skeleton-block skeleton-line skeleton-line--title" />
          <span className="skeleton-block skeleton-line skeleton-line--meta" />
          <span className="skeleton-block skeleton-line" />
          <span className="skeleton-block skeleton-line skeleton-line--short" />
        </div>
      </div>
      <section className="media-section" aria-hidden="true">
        <div className="section-heading">
          <span className="skeleton-block skeleton-line skeleton-line--heading" />
        </div>
        <div className="media-rail media-rail--skeleton">
          {Array.from({ length: 6 }, (_, index) => (
            <span className="skeleton-poster" key={index}>
              <span className="skeleton-block skeleton-poster__art" />
              <span className="skeleton-block skeleton-line" />
              <span className="skeleton-block skeleton-line skeleton-line--short" />
            </span>
          ))}
        </div>
      </section>
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
  const featureHeadingRef = useRef<HTMLHeadingElement>(null);
  const recentRef = useRef<HTMLElement>(null);
  const previewMode = !session;
  const feature = library.feature;
  const featureProgress = feature ? itemProgress(feature) : 0;

  useEffect(() => {
    if (feature) featureHeadingRef.current?.focus({ preventScroll: true });
  }, [feature]);

  const scrollToRecent = () => {
    recentRef.current?.scrollIntoView({ behavior: "smooth", block: "start" });
  };

  return (
    <>
      <header className="topbar">
        <span className="brand" aria-label="Emby 影库">
          <BrandMark />
          <span>
            EMBER
            <span className="brand__sub">EMBY DESKTOP</span>
          </span>
        </span>
        <nav className="main-nav" aria-label="主导航">
          <span className="main-nav__item is-active">影库</span>
          <span className="main-nav__divider" />
          <span className="main-nav__library">
            {library.view?.name ?? (previewMode ? "界面预览" : "读取中…")}
            <Icon name="chevron" size={14} />
          </span>
        </nav>
        <div className="topbar__right">
          <span className={`server-status${previewMode ? " is-preview" : ""}`}>
            <i />
            <span>
              {previewMode
                ? "预览模式"
                : session?.serverName || compactUrl(session?.serverUrl) || "Emby"}
            </span>
          </span>
          <span
            className="profile-mark"
            aria-label={session ? `当前用户：${session.userName}` : "预览模式"}
          >
            {session ? userInitial(session.userName) : "—"}
          </span>
          <button
            className="logout-button"
            type="button"
            onClick={onLogout}
            disabled={logoutPending}
            aria-label={previewMode ? "返回登录页" : "退出 Emby 登录"}
          >
            <Icon name={previewMode ? "arrow-left" : "logout"} size={14} />
            <span>
              {logoutPending ? "退出中…" : previewMode ? "返回登录" : "退出"}
            </span>
          </button>
        </div>
      </header>

      <main className="home-main">
        {!desktop && (
          <div className="preview-banner" role="status">
            <span className="preview-banner__tag">PREVIEW MODE</span>
            <span>预览模式：需要 Tauri 桌面环境连接 Emby。页面不会发起任何媒体库请求。</span>
          </div>
        )}

        {previewMode || !desktop ? (
          <section className="library-message" aria-labelledby="preview-title">
            <span className="eyebrow">EMBY DESKTOP · PREVIEW</span>
            <h1 id="preview-title">媒体库界面预览</h1>
            <p>
              当前运行在浏览器中，无法读取服务器数据。请在 Tauri 桌面客户端中登录
              Emby，以浏览真实影库并使用本机 mpv 播放。
            </p>
            <div className="preview-shell" aria-hidden="true">
              <span className="preview-shell__hero" />
              <span className="preview-shell__rail">
                {Array.from({ length: 5 }, (_, index) => (
                  <span key={index} />
                ))}
              </span>
            </div>
          </section>
        ) : library.status === "loading" || library.status === "idle" ? (
          <LibrarySkeleton />
        ) : library.status === "error" ? (
          <section className="library-state" aria-labelledby="library-error-title">
            <span className="eyebrow">LIBRARY UNAVAILABLE</span>
            <h1 id="library-error-title">媒体库暂时无法载入</h1>
            <ErrorNotice
              message={library.error ?? "无法加载媒体库，请重试。"}
              onRetry={library.reload}
              retryLabel="重新加载"
            />
          </section>
        ) : library.items.length === 0 || !feature ? (
          <section className="library-state" aria-labelledby="library-empty-title">
            <span className="eyebrow">EMPTY LIBRARY</span>
            <h1 id="library-empty-title">这里还没有电影</h1>
            <p>
              {library.view?.name ? `「${library.view.name}」` : "当前媒体库"}
              中没有找到电影条目。请在 Emby 服务端确认媒体库类型与扫描状态。
            </p>
            <button className="button button--quiet" type="button" onClick={library.reload}>
              <Icon name="refresh" size={15} />
              重新读取
            </button>
          </section>
        ) : (
          <>
            <section className="feature" aria-labelledby="feature-title">
              <MediaArtwork
                src={feature.backdropUrl ?? feature.posterUrl}
                alt={`《${feature.name}》背景图`}
                title={feature.name}
                className="feature__art"
                loading="eager"
              />
              <div className="feature__scrim" />
              <div className="feature__content">
                <span className="eyebrow">
                  <span className="eyebrow__rule" />
                  来自你的影库
                  <span className="eyebrow__latin">FEATURED FILM</span>
                </span>
                <p className="feature__kicker">
                  {itemEpisodeLabel(feature) ?? itemKindLabel(feature)}
                  {feature.seriesName ? ` · ${feature.seriesName}` : ""}
                </p>
                <h1 id="feature-title" ref={featureHeadingRef} tabIndex={-1}>
                  {feature.name}
                </h1>
                <p className="feature__meta">
                  {itemYearLabel(feature) && (
                    <>
                      {itemYearLabel(feature)}
                      <i />
                    </>
                  )}
                  {itemKindLabel(feature)}
                  <i />
                  {formatRuntimeTicks(feature.runTimeTicks)}
                  {featureProgress > 0 && (
                    <>
                      <i />
                      已观看 {Math.round(featureProgress * 100)}%
                    </>
                  )}
                </p>
                <p className="feature__summary">
                  {feature.overview?.trim() || "暂无简介。"}
                </p>
                <div className="feature__actions">
                  <button
                    className="button button--primary"
                    type="button"
                    onClick={() => onOpen(feature)}
                  >
                    <Icon name="play" size={16} />
                    {featureProgress > 0 ? "继续观看" : "查看详情"}
                  </button>
                  <button
                    className="button button--quiet"
                    type="button"
                    onClick={scrollToRecent}
                  >
                    全部影片 <Icon name="arrow-right" size={16} />
                  </button>
                </div>
              </div>
              <div className="feature__rail-mark">
                <span>01</span>
                <i />
                {String(library.items.length).padStart(2, "0")}
              </div>
            </section>

            {library.resumable.length > 0 && (
              <section className="media-section" aria-labelledby="continue-title">
                <div className="section-heading">
                  <div>
                    <span className="eyebrow">拾起上次的故事</span>
                    <h2 id="continue-title">继续观看</h2>
                  </div>
                  <span className="section-count">
                    {String(library.resumable.length).padStart(2, "0")} <i>/</i> RESUME
                  </span>
                </div>
                <div className="media-rail">
                  {library.resumable.map((item) => (
                    <MediaCard key={item.id} item={item} onSelect={onOpen} progress />
                  ))}
                </div>
              </section>
            )}

            <section
              className="media-section media-section--recent"
              aria-labelledby="recent-title"
              ref={recentRef}
            >
              <div className="section-heading section-heading--small">
                <div>
                  <span className="eyebrow">刚刚入库</span>
                  <h2 id="recent-title">最近添加</h2>
                </div>
                <span className="section-count">
                  {library.view?.name ?? "EMBY"} <i>·</i> {library.totalItems} 部
                </span>
              </div>
              <div className="media-rail media-rail--posters">
                {library.items.map((item) => (
                  <MediaCard key={item.id} item={item} onSelect={onOpen} />
                ))}
              </div>
            </section>
          </>
        )}

        <footer className="home-footer">
          <span>
            <BrandMark />
            {session ? `${session.userName} 的私人影库。` : "私人影库，静静相伴。"}
          </span>
          <span>
            {session
              ? `EMBY · ${library.view?.name ?? session.serverName}`
              : "EMBY DESKTOP · PREVIEW"}
          </span>
        </footer>
      </main>
    </>
  );
}
