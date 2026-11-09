import { useEffect, useRef, useState } from "react";
import type { EmbyPlaybackTarget, EmbySession } from "../emby/contract";
import type { EmbyLibraryItem } from "../emby/useEmby";
import {
  useEmbyDetail,
  useEmbyEpisodes,
  usePlaybackLauncher,
} from "../emby/useEmby";
import {
  episodePlaybackTitle,
  formatRuntimeTicks,
  itemEpisodeCode,
  itemEpisodeLabel,
  itemKindLabel,
  itemPlaybackTitle,
  itemProgress,
  itemYearLabel,
} from "../emby/format";
import { isPlayableItemType } from "../emby/library";
import { ErrorNotice, Icon, MediaArtwork } from "./shared";

interface DetailScreenProps {
  itemId: string;
  seed: EmbyLibraryItem | null;
  session: EmbySession | null;
  desktop: boolean;
  logoutPending: boolean;
  onBack: () => void;
  onPlay: (target: EmbyPlaybackTarget, title: string) => void;
  onLogout: () => void;
}

function DetailSkeleton() {
  return (
    <div className="detail-status" role="status" aria-live="polite">
      <span className="visually-hidden">正在读取条目详情…</span>
      <div className="detail-skeleton" aria-hidden="true">
        <span className="skeleton-poster detail-skeleton__poster" />
        <div className="detail-skeleton__info">
          <span className="skeleton-text skeleton-text--title" />
          <span className="skeleton-text skeleton-text--meta" />
          <span className="skeleton-text" />
          <span className="skeleton-text" />
          <span className="skeleton-text skeleton-text--short" />
        </div>
      </div>
    </div>
  );
}

function groupEpisodes(episodes: EmbyLibraryItem[]) {
  const groups = new Map<number | null, EmbyLibraryItem[]>();
  for (const episode of episodes) {
    const season = episode.parentIndexNumber;
    const existing = groups.get(season);
    if (existing) {
      existing.push(episode);
    } else {
      groups.set(season, [episode]);
    }
  }
  return Array.from(groups.entries());
}

function EpisodeList({
  episodes,
  seriesName,
  pendingId,
  playbackPending,
  onPlay,
}: {
  episodes: EmbyLibraryItem[];
  seriesName: string;
  pendingId: string | null;
  playbackPending: boolean;
  onPlay: (episode: EmbyLibraryItem) => void;
}) {
  const groups = groupEpisodes(episodes);

  return (
    <div className="episode-groups">
      {groups.map(([season, seasonEpisodes]) => (
        <section key={season ?? "unknown"}>
          <h3 className="season-title">
            {season === null ? "剧集" : `第 ${season} 季`}
          </h3>
          <div className="episode-list">
            {seasonEpisodes.map((episode) => {
              const progress = itemProgress(episode);
              const code =
                itemEpisodeCode(episode) ??
                itemEpisodeLabel(episode) ??
                "—";
              const active = pendingId === episode.id;
              const progressLabel =
                progress > 0
                  ? `${Math.round(progress * 100)}%`
                  : episode.userData.played
                    ? "已看完"
                    : "";

              return (
                <button
                  className="episode-row"
                  key={episode.id}
                  type="button"
                  disabled={playbackPending}
                  aria-label={`播放《${seriesName}》${code} ${episode.name}`}
                  onClick={() => onPlay(episode)}
                >
                  <span className="episode-row__code">{code}</span>
                  <span className="episode-row__title">{episode.name}</span>
                  <span className="episode-row__duration">
                    {formatRuntimeTicks(episode.runTimeTicks)}
                  </span>
                  <span className="episode-row__progress">
                    {active ? "正在准备…" : progressLabel}
                  </span>
                </button>
              );
            })}
          </div>
        </section>
      ))}
    </div>
  );
}

export default function DetailScreen({
  itemId,
  seed,
  session,
  desktop,
  logoutPending,
  onBack,
  onPlay,
  onLogout,
}: DetailScreenProps) {
  const detail = useEmbyDetail(itemId, seed, desktop && Boolean(session));
  const playback = usePlaybackLauncher();
  const headingRef = useRef<HTMLHeadingElement>(null);
  const [pendingEpisodeId, setPendingEpisodeId] = useState<string | null>(null);

  const item = detail.item;
  const progress = item ? itemProgress(item) : 0;
  const isSeries = item?.type === "Series";
  const episodes = useEmbyEpisodes(
    isSeries ? item.id : null,
    desktop && Boolean(session),
  );

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
    if (!item || playback.pending || !isPlayableItemType(item.type)) return;
    const target = await playback.launch(item.id);
    if (target) onPlay(target, itemPlaybackTitle(item));
  };

  const playEpisode = async (episode: EmbyLibraryItem) => {
    if (!item || playback.pending) return;
    setPendingEpisodeId(episode.id);
    const target = await playback.launch(episode.id);
    setPendingEpisodeId(null);
    if (target) {
      onPlay(target, episodePlaybackTitle(item.name, episode));
    }
  };

  const metaParts: string[] = [];
  if (item) {
    const year = itemYearLabel(item);
    if (year) metaParts.push(year);
    metaParts.push(itemKindLabel(item));
    const runtime = formatRuntimeTicks(item.runTimeTicks);
    if (runtime !== "时长未知") metaParts.push(runtime);
    const episodeLabel = itemEpisodeLabel(item);
    if (episodeLabel) metaParts.push(episodeLabel);
    if (item.seriesName) metaParts.push(item.seriesName);
    if (progress > 0) metaParts.push(`已观看 ${Math.round(progress * 100)}%`);
    if (item.userData.played) metaParts.push("已看完");
  }

  return (
    <>
      <header className="topbar">
        <div className="topbar__left">
          <button
            className="text-button"
            type="button"
            onClick={onBack}
            aria-label="返回媒体库"
          >
            <Icon name="arrow-left" size={16} />
            返回
          </button>
          <span className="topbar__divider" aria-hidden="true" />
          <span className="topbar__brand">Emby PC</span>
        </div>
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
            aria-label="退出 Emby 登录"
          >
            {logoutPending ? "退出中…" : "退出"}
          </button>
        </div>
      </header>

      <main className="detail-main">
        {!item && detail.loading && <DetailSkeleton />}

        {!item && !detail.loading && detail.error && (
          <section className="detail-status" aria-labelledby="detail-error-title">
            <h1 id="detail-error-title" className="detail-status__title">
              无法读取条目详情
            </h1>
            <ErrorNotice
              message={detail.error}
              onRetry={detail.reload}
              retryLabel="重新读取"
            />
            <button className="button button--secondary" type="button" onClick={onBack}>
              返回媒体库
            </button>
          </section>
        )}

        {!item && !detail.loading && !detail.error && (
          <section className="detail-status" aria-labelledby="detail-missing-title">
            <h1 id="detail-missing-title" className="detail-status__title">
              条目不存在
            </h1>
            <p className="status-line">服务器没有返回该条目，可能已被移出媒体库。</p>
            <button className="button button--secondary" type="button" onClick={onBack}>
              返回媒体库
            </button>
          </section>
        )}

        {item && (
          <>
            {detail.error && (
              <div className="detail-warning">
                <ErrorNotice
                  message={`${detail.error} 当前显示已加载的信息。`}
                  onRetry={detail.reload}
                  retryLabel="重新读取"
                />
              </div>
            )}

            <section
              className="detail-layout"
              aria-labelledby="detail-title"
              aria-busy={detail.loading}
            >
              <div className="detail-poster">
                <MediaArtwork
                  src={item.posterUrl ?? item.backdropUrl}
                  alt={`《${item.name}》海报`}
                  title={item.name}
                  loading="eager"
                />
              </div>

              <div className="detail-info">
                <h1 id="detail-title" className="detail-title" ref={headingRef} tabIndex={-1}>
                  {item.name}
                </h1>
                <p className="detail-meta">
                  {metaParts.map((part, index) => (
                    <span key={`${part}-${index}`}>
                      {index > 0 && <i aria-hidden="true" />}
                      {part}
                    </span>
                  ))}
                </p>
                <p className="detail-overview">
                  {item.overview?.trim() || "暂无简介。"}
                </p>
                {item.genres.length > 0 && (
                  <p className="detail-genres">{item.genres.join(" · ")}</p>
                )}

                <div className="detail-actions">
                  {isPlayableItemType(item.type) && (
                    <button
                      className="button button--primary"
                      type="button"
                      onClick={() => void startPlayback()}
                      disabled={playback.pending}
                    >
                      <Icon name="play" size={15} />
                      {playback.pending ? "正在准备…" : "播放"}
                    </button>
                  )}
                  {isSeries && (
                    <span className="detail-hint">在下方选择剧集播放</span>
                  )}
                  {item.type === "MusicAlbum" && (
                    <span className="detail-hint">音乐专辑暂不支持播放。</span>
                  )}
                  {progress > 0 && (
                    <span className="detail-progress">
                      已观看 {Math.round(progress * 100)}%
                    </span>
                  )}
                </div>

                {playback.error && (
                  <div className="detail-warning">
                    <ErrorNotice message={playback.error} />
                  </div>
                )}
              </div>
            </section>

            {isSeries && (
              <section className="episodes" aria-labelledby="episodes-title">
                <div className="section-header">
                  <h2 className="section-title" id="episodes-title">
                    剧集
                  </h2>
                  <span className="section-meta">
                    {episodes.items.length > 0 ? `${episodes.items.length} 集` : ""}
                  </span>
                </div>

                {episodes.loading && <p className="status-line">正在读取剧集…</p>}
                {episodes.error && (
                  <ErrorNotice
                    message={episodes.error}
                    onRetry={episodes.reload}
                    retryLabel="重新读取"
                  />
                )}
                {!episodes.loading && !episodes.error && episodes.items.length === 0 && (
                  <p className="empty-state">未找到剧集。</p>
                )}
                {episodes.items.length > 0 && (
                  <EpisodeList
                    episodes={episodes.items}
                    seriesName={item.name}
                    pendingId={pendingEpisodeId}
                    playbackPending={playback.pending}
                    onPlay={(episode) => void playEpisode(episode)}
                  />
                )}
              </section>
            )}
          </>
        )}
      </main>
    </>
  );
}
