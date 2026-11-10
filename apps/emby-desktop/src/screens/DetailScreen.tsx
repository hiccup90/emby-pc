import { useEffect, useMemo, useRef, useState } from "react";
import type { EmbyPlaybackTarget, EmbySession } from "../emby/contract";
import type { PlaybackMeta } from "../player/contract";
import type { EmbyLibraryItem } from "../emby/useEmby";
import { useEmbyDetail, useEmbyEpisodes, usePlaybackLauncher } from "../emby/useEmby";
import {
  episodePlaybackMeta,
  formatRemainingTicks,
  formatRuntimeTicks,
  itemEpisodeLabel,
  itemPlaybackMeta,
  itemProgress,
  itemYearLabel,
} from "../emby/format";
import { isPlayableItemType } from "../emby/library";
import { ErrorNotice, Icon, MediaArtwork, ProgressBar } from "./shared";

interface DetailScreenProps {
  itemId: string;
  seed: EmbyLibraryItem | null;
  session: EmbySession | null;
  desktop: boolean;
  onBack: () => void;
  onPlay: (target: EmbyPlaybackTarget, meta: PlaybackMeta) => void;
  /** The shell draws the backdrop under the scroll container. */
  onBackdrop?: (src: string | null) => void;
}

function DetailSkeleton() {
  return (
    <div className="detail-hero" role="status" aria-live="polite">
      <span className="visually-hidden">正在读取条目详情…</span>
      <span className="detail-art skeleton-block" aria-hidden="true" />
      <div className="detail-info" aria-hidden="true">
        <span className="skeleton-line skeleton-line--title" />
        <span className="skeleton-line skeleton-line--meta" />
        <span className="skeleton-line skeleton-line--button" />
        <span className="skeleton-line" />
        <span className="skeleton-line" />
        <span className="skeleton-line skeleton-line--short" />
      </div>
    </div>
  );
}

interface SeasonGroup {
  key: string;
  label: string;
  episodes: EmbyLibraryItem[];
}

function groupSeasons(episodes: EmbyLibraryItem[]): SeasonGroup[] {
  const groups = new Map<string, SeasonGroup>();
  for (const episode of episodes) {
    const season = episode.parentIndexNumber;
    const key = season === null ? "none" : String(season);
    let group = groups.get(key);
    if (!group) {
      group = {
        key,
        label: season === null ? "其他" : season === 0 ? "特别篇" : `第 ${season} 季`,
        episodes: [],
      };
      groups.set(key, group);
    }
    group.episodes.push(episode);
  }
  return Array.from(groups.values());
}

/** First season that still has something to watch, like Plex's "On Deck". */
function defaultSeason(groups: SeasonGroup[]): string | null {
  const next = groups.find((group) => group.episodes.some((episode) => !episode.userData.played));
  return (next ?? groups[0])?.key ?? null;
}

function EpisodeSection({
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
  const groups = useMemo(() => groupSeasons(episodes), [episodes]);
  const [selected, setSelected] = useState<string | null>(null);
  const activeKey = groups.some((group) => group.key === selected) ? selected : defaultSeason(groups);
  const active = groups.find((group) => group.key === activeKey) ?? groups[0];
  if (!active) return null;

  return (
    <>
      {groups.length > 1 && (
        <div className="season-tabs" role="group" aria-label="选择季">
          {groups.map((group) => (
            <button
              key={group.key}
              type="button"
              aria-pressed={group.key === active.key}
              className={`season-tab${group.key === active.key ? " is-active" : ""}`}
              onClick={() => setSelected(group.key)}
            >
              {group.label}
              <span className="season-tab__count">{group.episodes.length}</span>
            </button>
          ))}
        </div>
      )}
      <ol className="episode-list" aria-label={active.label}>
        {active.episodes.map((episode) => {
          const progress = itemProgress(episode);
          const pending = pendingId === episode.id;
          const label = itemEpisodeLabel(episode) ?? "";
          const meta = [label, formatRuntimeTicks(episode.runTimeTicks)].filter(
            (part) => part && part !== "时长未知",
          );
          return (
            <li key={episode.id}>
              <button
                type="button"
                className={`episode-row${pending ? " is-pending" : ""}`}
                disabled={playbackPending}
                aria-label={`播放《${seriesName}》${label} ${episode.name}`}
                onClick={() => onPlay(episode)}
              >
                <span className="episode-row__index" aria-hidden="true">
                  <span className="episode-row__number">{episode.indexNumber ?? "·"}</span>
                  <span className="episode-row__play">
                    <Icon name="play" size={16} />
                  </span>
                </span>
                <span className="episode-row__body">
                  <span className="episode-row__title">{episode.name}</span>
                  <span className="episode-row__meta">{meta.join(" · ")}</span>
                  {episode.overview && <span className="episode-row__overview">{episode.overview}</span>}
                  {progress > 0 && <ProgressBar value={progress} className="episode-row__progress" />}
                </span>
                <span className="episode-row__state">
                  {pending ? (
                    <span className="spinner spinner--small" aria-label="正在准备" />
                  ) : episode.userData.played ? (
                    <span className="watched-mark" title="已看完">
                      <Icon name="check" size={14} />
                      <span className="visually-hidden">已看完</span>
                    </span>
                  ) : progress > 0 ? (
                    `${Math.round(progress * 100)}%`
                  ) : null}
                </span>
              </button>
            </li>
          );
        })}
      </ol>
    </>
  );
}

export default function DetailScreen({
  itemId,
  seed,
  session,
  desktop,
  onBack,
  onPlay,
  onBackdrop,
}: DetailScreenProps) {
  const detail = useEmbyDetail(itemId, seed, desktop && Boolean(session));
  const playback = usePlaybackLauncher();
  const headingRef = useRef<HTMLHeadingElement>(null);
  const [pendingEpisodeId, setPendingEpisodeId] = useState<string | null>(null);

  const item = detail.item;
  const progress = item ? itemProgress(item) : 0;
  const isSeries = item?.type === "Series";
  const episodes = useEmbyEpisodes(isSeries ? item.id : null, desktop && Boolean(session));
  const landscapeArt = item?.type === "Episode" || item?.type === "Video";
  const backdrop = item ? item.backdropUrl ?? (landscapeArt ? item.posterUrl : null) : null;

  useEffect(() => {
    onBackdrop?.(backdrop);
  }, [backdrop, onBackdrop]);
  useEffect(() => () => onBackdrop?.(null), [onBackdrop]);

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
    if (target) onPlay(target, itemPlaybackMeta(item));
  };

  const playEpisode = async (episode: EmbyLibraryItem) => {
    if (!item || playback.pending) return;
    setPendingEpisodeId(episode.id);
    const target = await playback.launch(episode.id);
    setPendingEpisodeId(null);
    if (target) onPlay(target, episodePlaybackMeta(item.name, episode));
  };

  if (!item) {
    return (
      <main className="page detail-page">
        {detail.loading ? (
          <DetailSkeleton />
        ) : (
          <section className="page-message" aria-labelledby="detail-status-title">
            <h1 id="detail-status-title" className="page-title">
              {detail.error ? "无法读取条目详情" : "条目不存在"}
            </h1>
            {detail.error ? (
              <ErrorNotice message={detail.error} onRetry={detail.reload} retryLabel="重新读取" />
            ) : (
              <p className="status-line">服务器没有返回该条目，可能已被移出媒体库。</p>
            )}
            <button className="button button--secondary" type="button" onClick={onBack}>
              返回媒体库
            </button>
          </section>
        )}
      </main>
    );
  }

  const episode = item.type === "Episode";
  const playable = isPlayableItemType(item.type);
  const runtime = formatRuntimeTicks(item.runTimeTicks);
  const seasonCount = isSeries ? groupSeasons(episodes.items).length : 0;

  // Episode pages read like Plex Web: series, season, "第 8 集 · name".
  const seasonLabel =
    item.parentIndexNumber === null ? null : item.parentIndexNumber === 0 ? "特别篇" : `第 ${item.parentIndexNumber} 季`;
  const episodeLine = item.indexNumber === null ? item.name : `第 ${item.indexNumber} 集 · ${item.name}`;

  const metaParts: string[] = [];
  const year = itemYearLabel(item);
  if (year) metaParts.push(year);
  if (runtime !== "时长未知") metaParts.push(runtime);
  if (isSeries && episodes.items.length > 0) {
    metaParts.push(`${seasonCount} 季`, `${episodes.items.length} 集`);
  }

  const remaining = formatRemainingTicks(item);

  return (
    <main className="page detail-page">
      {detail.error && (
        <div className="page-message page-message--inline">
          <ErrorNotice message={`${detail.error} 当前显示已加载的信息。`} onRetry={detail.reload} retryLabel="重新读取" />
        </div>
      )}

      <section
        className={`detail-hero${landscapeArt ? " detail-hero--landscape" : ""}`}
        aria-labelledby="detail-title"
        aria-busy={detail.loading}
      >
        <div className={`detail-art${landscapeArt ? " detail-art--landscape" : ""}`}>
          <MediaArtwork src={item.posterUrl ?? item.backdropUrl} alt={`《${item.name}》海报`} title={item.name} loading="eager" />
          {progress > 0 && <ProgressBar value={progress} className="detail-art__progress" />}
        </div>

        <div className="detail-info">
          <h1 id="detail-title" className="detail-title" ref={headingRef} tabIndex={-1}>
            {episode && item.seriesName ? item.seriesName : item.name}
          </h1>
          {episode && item.seriesName && (
            <>
              {seasonLabel && <p className="detail-season">{seasonLabel}</p>}
              <p className="detail-subtitle">{episodeLine}</p>
            </>
          )}
          {(metaParts.length > 0 || item.userData.played) && (
            <p className="detail-meta">
              {metaParts.map((part, index) => (
                <span key={`${part}-${index}`}>{part}</span>
              ))}
              {item.userData.played && <span className="chip">已看完</span>}
            </p>
          )}
          {item.genres.length > 0 && <p className="detail-genres">{item.genres.join(" · ")}</p>}

          <div className="detail-actions">
            {playable && (
              <button
                className="button button--primary button--large"
                type="button"
                onClick={() => void startPlayback()}
                disabled={playback.pending}
              >
                {playback.pending ? <span className="spinner spinner--small spinner--dark" aria-hidden="true" /> : <Icon name="play" size={18} />}
                {playback.pending ? "正在准备…" : progress > 0 ? "继续播放" : "播放"}
              </button>
            )}
            {playable && progress > 0 && (
              <span className="detail-resume">
                <ProgressBar value={progress} />
                {remaining ?? `已观看 ${Math.round(progress * 100)}%`}
              </span>
            )}
            {isSeries && <span className="detail-hint">在下方选择剧集播放</span>}
            {item.type === "MusicAlbum" && <span className="detail-hint">音乐专辑暂不支持播放。</span>}
          </div>

          {playback.error && <ErrorNotice message={playback.error} />}

          <p className="detail-overview">{item.overview?.trim() || "暂无简介。"}</p>
        </div>
      </section>

      {isSeries && (
        <section className="episodes" aria-labelledby="episodes-title">
          <div className="section-header">
            <h2 className="section-title" id="episodes-title">
              剧集
            </h2>
          </div>
          {episodes.loading && (
            <p className="status-line">
              <span className="spinner spinner--small" aria-hidden="true" /> 正在读取剧集…
            </p>
          )}
          {episodes.error && <ErrorNotice message={episodes.error} onRetry={episodes.reload} retryLabel="重新读取" />}
          {!episodes.loading && !episodes.error && episodes.items.length === 0 && <p className="empty-state">未找到剧集。</p>}
          {episodes.items.length > 0 && (
            <EpisodeSection
              episodes={episodes.items}
              seriesName={item.name}
              pendingId={pendingEpisodeId}
              playbackPending={playback.pending}
              onPlay={(next) => void playEpisode(next)}
            />
          )}
        </section>
      )}
    </main>
  );
}
