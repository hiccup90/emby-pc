import { useEffect, useMemo, useRef, useState } from "react";
import type { EmbyPerson, EmbyPlaybackTarget, EmbySession, EmbyUserData } from "../emby/contract";
import type { PlaybackMeta } from "../player/contract";
import type { EmbyLibraryItem } from "../emby/useEmby";
import { useEmbyDetail, useEmbyEpisodes, useEmbySimilar, usePlaybackLauncher } from "../emby/useEmby";
import { describeEmbyError, embySetFavorite, embySetPlayed } from "../emby/client";
import {
  audioStreamSummary,
  episodePlaybackMeta,
  formatCommunityRating,
  formatPremiereDate,
  formatRemainingTicks,
  formatRuntimeTicks,
  itemEpisodeLabel,
  itemPlaybackMeta,
  itemProgress,
  itemChapters,
  itemYearLabel,
  seriesStatusLabel,
  subtitleStreamSummary,
  videoStreamSummary,
} from "../emby/format";
import { isPlayableItemType } from "../emby/library";
import { HubRow, PosterCard } from "./cards";
import { ErrorNotice, Icon, MediaArtwork, ProgressBar } from "./shared";

interface DetailScreenProps {
  itemId: string;
  seed: EmbyLibraryItem | null;
  session: EmbySession | null;
  desktop: boolean;
  onBack: () => void;
  /**
   * Start playback. `queue` lists the items to auto-play in order (starting
   * with the one just opened); `trackScope` keys the remembered track choice.
   */
  onPlay: (
    target: EmbyPlaybackTarget,
    meta: PlaybackMeta,
    queue?: EmbyLibraryItem[],
    trackScope?: string,
  ) => void;
  /** The shell draws the backdrop under the scroll container. */
  onBackdrop?: (src: string | null) => void;
  /** A "more like this" poster. */
  onOpenItem: (item: EmbyLibraryItem) => void;
  /** A cast member: open their filmography. */
  onOpenPerson?: (person: EmbyPerson) => void;
}

/** `favorite` is not in the wire contract yet; reading it optionally keeps this forward-compatible. */
type UserDataWithFavorite = EmbyUserData & { favorite?: boolean | null };

const CREW_ORDER = ["Director", "Writer", "Producer", "Composer"];
const ROLE_LABELS: Record<string, string> = {
  Director: "导演",
  Writer: "编剧",
  Producer: "制片",
  Composer: "作曲",
  GuestStar: "客串",
};

/** Plex's "Cast & Crew": actors in server order, then the crew. */
function castAndCrew(people: EmbyPerson[]): EmbyPerson[] {
  const actors = people.filter((person) => person.kind === "Actor" || person.kind === "GuestStar");
  const crew = CREW_ORDER.flatMap((kind) => people.filter((person) => person.kind === kind));
  const seen = new Set<string>();
  return [...actors, ...crew]
    .filter((person) => {
      const key = `${person.id ?? person.name}|${person.kind}`;
      if (seen.has(key)) return false;
      seen.add(key);
      return true;
    })
    .slice(0, 30);
}

function personRole(person: EmbyPerson): string | null {
  if (person.kind === "Actor") return person.role;
  if (person.kind === "GuestStar") return person.role ?? ROLE_LABELS.GuestStar;
  return ROLE_LABELS[person.kind ?? ""] ?? null;
}

function HeartIcon({ filled = false, size = 16 }: { filled?: boolean; size?: number }) {
  return (
    <svg
      aria-hidden="true"
      width={size}
      height={size}
      viewBox="0 0 24 24"
      fill={filled ? "currentColor" : "none"}
      stroke="currentColor"
      strokeWidth={1.7}
      strokeLinecap="round"
      strokeLinejoin="round"
    >
      <path d="M20.8 6.6a5.5 5.5 0 0 0-7.8 0L12 7.6l-1-1a5.5 5.5 0 0 0-7.8 7.8l1 1L12 23l7.8-7.6 1-1a5.5 5.5 0 0 0 0-7.8z" />
    </svg>
  );
}

function initials(name: string): string {
  const trimmed = name.trim();
  if (!trimmed) return "?";
  if (/[\u2e80-\u9fff]/.test(trimmed)) return Array.from(trimmed)[0];
  return trimmed
    .split(/\s+/)
    .slice(0, 2)
    .map((part) => part[0]?.toUpperCase() ?? "")
    .join("");
}

function PersonCard({
  person,
  photo,
  onOpen,
}: {
  person: EmbyPerson;
  photo: string | undefined;
  onOpen?: () => void;
}) {
  const [failed, setFailed] = useState(false);
  const role = personRole(person);
  const label = role ? `${person.name} · ${role}` : person.name;
  return (
    <div
      className={`person-card${onOpen ? " person-card--link" : ""}`}
      title={onOpen ? `${label}（查看作品）` : label}
      role={onOpen ? "button" : undefined}
      tabIndex={onOpen ? 0 : undefined}
      onClick={onOpen}
      onKeyDown={
        onOpen
          ? (event) => {
              if (event.key === "Enter" || event.key === " ") {
                event.preventDefault();
                onOpen();
              }
            }
          : undefined
      }
    >
      <span className="person-card__photo" aria-hidden="true">
        {photo && !failed ? (
          <img src={photo} alt="" loading="lazy" decoding="async" onError={() => setFailed(true)} />
        ) : (
          <span className="person-card__initials">{initials(person.name)}</span>
        )}
      </span>
      <span className="person-card__name">{person.name}</span>
      <span className="person-card__role">{role ?? "\u00a0"}</span>
    </div>
  );
}

function namesOf(people: EmbyPerson[], kind: string): string | null {
  const names = Array.from(new Set(people.filter((person) => person.kind === kind).map((person) => person.name)));
  return names.length > 0 ? names.slice(0, 3).join("、") : null;
}

/** Plex's details grid: credits on the left, media on the right. */
function DetailFacts({ item }: { item: EmbyLibraryItem }) {
  const isSeries = item.type === "Series";
  const credits: Array<[string, string | null]> = [
    ["导演", namesOf(item.people, "Director")],
    ["编剧", namesOf(item.people, "Writer")],
    ["出品", item.studios.length > 0 ? item.studios.slice(0, 3).join("、") : null],
    [isSeries ? "首播" : item.type === "Episode" ? "播出" : "上映", formatPremiereDate(item.premiereDate)],
    ["状态", isSeries ? seriesStatusLabel(item.status) : null],
  ];
  const media: Array<[string, string | null]> = [
    ["视频", videoStreamSummary(item.mediaStreams)],
    ["音频", audioStreamSummary(item.mediaStreams)],
    ["字幕", subtitleStreamSummary(item.mediaStreams)],
  ];
  const groups = [credits, media]
    .map((rows) => rows.filter((row): row is [string, string] => Boolean(row[1])))
    .filter((rows) => rows.length > 0);
  if (groups.length === 0) return null;
  return (
    <div className="detail-facts">
      {groups.map((rows, index) => (
        <dl className="detail-facts__group" key={index}>
          {rows.map(([label, value]) => (
            <div className="detail-facts__row" key={label}>
              <dt>{label}</dt>
              <dd>{value}</dd>
            </div>
          ))}
        </dl>
      ))}
    </div>
  );
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
  onOpenItem,
  onOpenPerson,
}: DetailScreenProps) {
  const detail = useEmbyDetail(itemId, seed, desktop && Boolean(session));
  const playback = usePlaybackLauncher();
  const headingRef = useRef<HTMLHeadingElement>(null);
  const [pendingEpisodeId, setPendingEpisodeId] = useState<string | null>(null);
  const [watchedOverride, setWatchedOverride] = useState<boolean | null>(null);
  const [favoriteOverride, setFavoriteOverride] = useState<boolean | null>(null);
  const [pendingAction, setPendingAction] = useState<"played" | "favorite" | null>(null);
  const [actionError, setActionError] = useState<string | null>(null);

  const item = detail.item;
  const progress = item ? itemProgress(item) : 0;
  const effectiveWatched = watchedOverride ?? item?.userData.played ?? false;
  const favoriteFromServer = (item?.userData as UserDataWithFavorite | undefined)?.favorite;
  const favoriteState =
    favoriteOverride ?? (typeof favoriteFromServer === "boolean" ? favoriteFromServer : null);
  const isSeries = item?.type === "Series";
  const episodes = useEmbyEpisodes(isSeries ? item.id : null, desktop && Boolean(session));
  const similar = useEmbySimilar(
    item && (item.type === "Movie" || item.type === "Series") ? item.id : null,
    desktop && Boolean(session),
  );
  const landscapeArt = item?.type === "Episode" || item?.type === "Video";
  const backdrop = item ? item.backdropUrl ?? (landscapeArt ? item.posterUrl : null) : null;

  useEffect(() => {
    onBackdrop?.(backdrop);
  }, [backdrop, onBackdrop]);
  useEffect(() => () => onBackdrop?.(null), [onBackdrop]);

  useEffect(() => {
    if (item) headingRef.current?.focus({ preventScroll: true });
  }, [item]);

  // Local toggles belong to one detail item; drop them when the page changes.
  useEffect(() => {
    setWatchedOverride(null);
    setFavoriteOverride(null);
    setPendingAction(null);
    setActionError(null);
  }, [itemId]);

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
    if (target) {
      onPlay(target, { ...itemPlaybackMeta(item), chapters: itemChapters(item) }, [item], item.id);
    }
  };

  const playEpisode = async (episode: EmbyLibraryItem) => {
    if (!item || playback.pending) return;
    setPendingEpisodeId(episode.id);
    const target = await playback.launch(episode.id);
    setPendingEpisodeId(null);
    if (!target) return;
    // Episodes after this one follow automatically; track choice is per series.
    const list = episodes.items.length > 0 ? episodes.items : [episode];
    const index = list.findIndex((entry) => entry.id === episode.id);
    const queue = index >= 0 ? list.slice(index) : [episode];
    onPlay(
      target,
      { ...episodePlaybackMeta(item.name, episode), chapters: itemChapters(episode) },
      queue,
      item.id,
    );
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
  const rating = formatCommunityRating(item.communityRating);
  const cast = castAndCrew(item.people);
  const supportsWatchState = isSeries || playable;

  const toggleWatched = async () => {
    if (pendingAction) return;
    const next = !effectiveWatched;
    setPendingAction("played");
    setActionError(null);
    try {
      await embySetPlayed(item.id, next);
      setWatchedOverride(next);
      // Re-read the item so counts/progress reflect the server state.
      detail.reload();
    } catch (reason) {
      setActionError(describeEmbyError(reason, "更新观看状态失败，请稍后重试。"));
    } finally {
      setPendingAction(null);
    }
  };

  const toggleFavorite = async () => {
    if (pendingAction) return;
    const next = favoriteState !== true;
    setPendingAction("favorite");
    setActionError(null);
    try {
      await embySetFavorite(item.id, next);
      setFavoriteOverride(next);
      // The contract has no favorite field yet; refetch in case the API adds it.
      detail.reload();
    } catch (reason) {
      setActionError(describeEmbyError(reason, "更新收藏状态失败，请稍后重试。"));
    } finally {
      setPendingAction(null);
    }
  };

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
          {(metaParts.length > 0 || rating || item.officialRating || effectiveWatched) && (
            <p className="detail-meta">
              {metaParts.map((part, index) => (
                <span key={`${part}-${index}`}>{part}</span>
              ))}
              {rating && (
                <span className="detail-rating" title="评分">
                  <span aria-hidden="true">★</span> {rating}
                </span>
              )}
              {item.officialRating && <span className="chip">{item.officialRating}</span>}
              {effectiveWatched && <span className="chip">已看完</span>}
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
            {supportsWatchState && (
              <>
                <button
                  type="button"
                  className={`detail-action-toggle${effectiveWatched ? " is-active" : ""}`}
                  aria-pressed={effectiveWatched}
                  aria-label={effectiveWatched ? "标记为未看" : "标记为已看"}
                  title={effectiveWatched ? "标记为未看" : "标记为已看"}
                  onClick={() => void toggleWatched()}
                  disabled={pendingAction !== null}
                >
                  {pendingAction === "played" ? (
                    <span className="spinner spinner--small" aria-hidden="true" />
                  ) : (
                    <Icon name="check" size={16} />
                  )}
                  {effectiveWatched ? "已看" : "标记为已看"}
                </button>
                <button
                  type="button"
                  className={`detail-action-toggle${favoriteState === true ? " is-active" : ""}`}
                  aria-pressed={favoriteState === true}
                  aria-label={favoriteState === true ? "取消收藏" : "收藏"}
                  title={favoriteState === true ? "取消收藏" : "收藏"}
                  onClick={() => void toggleFavorite()}
                  disabled={pendingAction !== null}
                >
                  {pendingAction === "favorite" ? (
                    <span className="spinner spinner--small" aria-hidden="true" />
                  ) : (
                    <HeartIcon filled={favoriteState === true} />
                  )}
                  {favoriteState === true ? "已收藏" : "收藏"}
                </button>
              </>
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
          {actionError && <ErrorNotice message={actionError} />}

          {item.tagline && <p className="detail-tagline">{item.tagline}</p>}
          <p className="detail-overview">{item.overview?.trim() || "暂无简介。"}</p>
          <DetailFacts item={item} />
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

      {(cast.length > 0 || similar.items.length > 0) && (
        <div className="detail-rows">
          {cast.length > 0 && (
            <HubRow title="演职人员" count={cast.length} variant="person">
              {cast.map((person, index) => (
                <PersonCard
                  key={`${person.id ?? person.name}-${person.kind}-${index}`}
                  person={person}
                  photo={person.id ? detail.photos[person.id] : undefined}
                  onOpen={
                    onOpenPerson && person.id
                      ? () => onOpenPerson(person)
                      : undefined
                  }
                />
              ))}
            </HubRow>
          )}
          {similar.items.length > 0 && (
            <HubRow title="更多类似内容" count={similar.items.length} variant="poster">
              {similar.items.map((other) => (
                <PosterCard key={other.id} item={other} landscape={false} onSelect={onOpenItem} />
              ))}
            </HubRow>
          )}
        </div>
      )}
    </main>
  );
}
