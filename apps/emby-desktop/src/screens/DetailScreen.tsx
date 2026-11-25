import { useEffect, useMemo, useRef, useState } from "react";
import type { KeyboardEvent as ReactKeyboardEvent } from "react";
import type { EmbyItem, EmbyMediaStream, EmbyPerson, EmbyPlaybackTarget, EmbySession } from "../emby/contract";
import type { PlaybackMeta } from "../player/contract";
import { findTrackIndexByChoice, readTrackPreference, writeTrackPreference } from "../player/trackPreference";
import type { TrackChoice, TrackPreference } from "../player/trackPreference";
import type { EmbyLibraryItem } from "../emby/useEmby";
import { useEmbyDetail, useEmbyEpisodes, useEmbySimilar, usePlaybackLauncher } from "../emby/useEmby";
import { useAsyncResource } from "../emby/useAsyncResource";
import { safeImageUrl } from "../emby/artwork";
import { describeEmbyError, embyItem, embySetFavorite, embySetPlayed } from "../emby/client";
import {
  audioStreamSummary,
  mediaStreamLabel,
  playbackMeta,
  formatCommunityRating,
  formatPremiereDate,
  formatRemainingTicks,
  formatRuntimeTicks,
  itemEpisodeLabel,
  itemProgress,
  itemStartSeconds,
  itemYearLabel,
  seriesStatusLabel,
  subtitleStreamSummary,
  videoStreamSummary,
} from "../emby/format";
import { isPlayableItemType } from "../emby/library";
import { HubRow, PosterCard } from "./cards";
import { ErrorNotice, Icon, MediaArtwork, ProgressBar, useEscape } from "./shared";

interface DetailScreenProps {
  itemId: string;
  seed: EmbyLibraryItem | null;
  /** Episode to mark as current after returning from the player (series pages only). */
  focusEpisodeId?: string | null;
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
  /** A "more like this" poster. */
  onOpenItem: (item: EmbyLibraryItem) => void;
  /** A cast member: open their filmography. */
  onOpenPerson?: (person: EmbyPerson) => void;
}

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

/** Compact "S1E8" for the series resume button; falls back to the name. */
function compactEpisodeLabel(episode: EmbyLibraryItem): string {
  const { parentIndexNumber, indexNumber } = episode;
  if (parentIndexNumber === null || indexNumber === null) return episode.name;
  return `S${parentIndexNumber}E${indexNumber}`;
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

function streamChoice(stream: EmbyMediaStream): TrackChoice {
  return { lang: stream.language, title: mediaStreamLabel(stream) };
}

function findStream(streams: EmbyMediaStream[], choice: TrackChoice): EmbyMediaStream | null {
  const index = findTrackIndexByChoice(streams.map(streamChoice), choice);
  return index >= 0 ? streams[index] : null;
}

/** A small track menu beside the trigger; the cinema hero clips dropdowns. */
function TrackMenu({
  label,
  value,
  options,
  onChange,
}: {
  label: string;
  value: string;
  options: ReadonlyArray<{ value: string; label: string }>;
  onChange: (value: string) => void;
}) {
  const [open, setOpen] = useState(false);
  const rootRef = useRef<HTMLDivElement>(null);
  const buttonRef = useRef<HTMLButtonElement>(null);
  const selectedIndex = Math.max(
    0,
    options.findIndex((option) => option.value === value),
  );
  const current = options[selectedIndex]?.label ?? "";

  useEffect(() => {
    if (!open) return undefined;
    const onPointerDown = (event: PointerEvent) => {
      if (rootRef.current?.contains(event.target as Node)) return;
      setOpen(false);
    };
    const onKeyDown = (event: KeyboardEvent) => {
      if (event.key !== "Escape") return;
      // Capture Escape so the detail page's back handler does not fire too.
      event.preventDefault();
      event.stopPropagation();
      setOpen(false);
      buttonRef.current?.focus();
    };
    const onScroll = () => setOpen(false);
    document.addEventListener("pointerdown", onPointerDown);
    document.addEventListener("keydown", onKeyDown, true);
    window.addEventListener("scroll", onScroll, true);
    return () => {
      document.removeEventListener("pointerdown", onPointerDown);
      document.removeEventListener("keydown", onKeyDown, true);
      window.removeEventListener("scroll", onScroll, true);
    };
  }, [open]);

  const moveFocus = (event: ReactKeyboardEvent<HTMLDivElement>) => {
    const keys = ["ArrowDown", "ArrowUp", "Home", "End"];
    if (!keys.includes(event.key)) return;
    const items = Array.from(event.currentTarget.querySelectorAll<HTMLButtonElement>("button"));
    if (items.length === 0) return;
    const currentIndex = items.indexOf(document.activeElement as HTMLButtonElement);
    const next =
      event.key === "Home"
        ? 0
        : event.key === "End"
          ? items.length - 1
          : event.key === "ArrowDown"
            ? (currentIndex + 1 + items.length) % items.length
            : (currentIndex - 1 + items.length) % items.length;
    event.preventDefault();
    items[next]?.focus();
  };

  return (
    <div
      className="detail-track"
      ref={rootRef}
      onBlur={(event) => {
        if (event.currentTarget.contains(event.relatedTarget as Node | null)) return;
        setOpen(false);
      }}
    >
      <span className="detail-track__label">{label}</span>
      <div className="detail-track__menu">
        <button
          ref={buttonRef}
          type="button"
          className="detail-track__button"
          aria-haspopup="menu"
          aria-expanded={open}
          aria-label={`${label}：${current}`}
          title={`${label}：${current}`}
          onClick={() => setOpen((value) => !value)}
        >
          <span className="detail-track__value">{current}</span>
          <Icon name="chevron-down" size={14} />
        </button>
        {open && (
          <div
            className="detail-track__options"
            role="menu"
            aria-label={label}
            onKeyDown={moveFocus}
          >
            {options.map((option, index) => {
              const selected = option.value === value;
              return (
                <button
                  key={option.value}
                  type="button"
                  role="menuitemradio"
                  aria-checked={selected}
                  autoFocus={index === selectedIndex}
                  className={`detail-track__option${selected ? " is-selected" : ""}`}
                  title={option.label}
                  onClick={() => {
                    onChange(option.value);
                    setOpen(false);
                    buttonRef.current?.focus();
                  }}
                >
                  <span>{option.label}</span>
                  {selected && <Icon name="check" size={15} />}
                </button>
              );
            })}
          </div>
        )}
      </div>
    </div>
  );
}

/** Audio/subtitle choices for the next play, remembered per movie/series. */
function TrackPickers({ scope, streams }: { scope: string; streams: EmbyMediaStream[] }) {
  const audioStreams = useMemo(() => streams.filter((stream) => stream.kind === "Audio"), [streams]);
  const subtitleStreams = useMemo(
    () => streams.filter((stream) => stream.kind === "Subtitle"),
    [streams],
  );
  const [version, setVersion] = useState(0);
  const preference = useMemo(() => readTrackPreference(scope), [scope, version]);

  const preferredAudio = preference?.audio ? findStream(audioStreams, preference.audio) : null;
  const selectedAudio =
    preferredAudio ?? audioStreams.find((stream) => stream.isDefault) ?? audioStreams[0] ?? null;
  const wantedSubtitle =
    preference && "subtitle" in preference ? preference.subtitle : undefined;
  const preferredSubtitle =
    wantedSubtitle === null
      ? null
      : wantedSubtitle
        ? findStream(subtitleStreams, wantedSubtitle)
        : undefined;
  const selectedSubtitle =
    preferredSubtitle === null
      ? null
      : preferredSubtitle ?? subtitleStreams.find((stream) => stream.isDefault) ?? null;

  if (audioStreams.length <= 1 && subtitleStreams.length === 0) return null;

  const audioIndex = selectedAudio ? audioStreams.indexOf(selectedAudio) : -1;
  const subtitleIndex = selectedSubtitle ? subtitleStreams.indexOf(selectedSubtitle) : -1;

  const choose = (next: TrackPreference) => {
    writeTrackPreference(scope, next);
    setVersion((value) => value + 1);
  };

  const audioOptions = audioStreams.map((stream, index) => ({
    value: String(index),
    label: mediaStreamLabel(stream),
  }));
  const subtitleOptions = [
    { value: "off", label: "关闭" },
    ...subtitleStreams.map((stream, index) => ({
      value: String(index),
      label: mediaStreamLabel(stream),
    })),
  ];

  return (
    <div className="detail-tracks" title="播放时使用，并按本片/本剧记住选择">
      {audioStreams.length > 1 && (
        <TrackMenu
          label="音轨"
          value={String(Math.max(0, audioIndex))}
          options={audioOptions}
          onChange={(next) => {
            const stream = audioStreams[Number(next)];
            if (!stream) return;
            choose({ ...(preference ?? {}), audio: streamChoice(stream) });
          }}
        />
      )}
      {subtitleStreams.length > 0 && (
        <TrackMenu
          label="字幕"
          value={subtitleIndex >= 0 ? String(subtitleIndex) : "off"}
          options={subtitleOptions}
          onChange={(next) => {
            if (next === "off") {
              choose({ ...(preference ?? {}), subtitle: null });
              return;
            }
            const stream = subtitleStreams[Number(next)];
            if (!stream) return;
            choose({ ...(preference ?? {}), subtitle: streamChoice(stream) });
          }}
        />
      )}
    </div>
  );
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
    <div className="detail-cinema detail-cinema--skeleton" role="status" aria-live="polite">
      <span className="visually-hidden">正在读取条目详情…</span>
      <div className="detail-cinema__content" aria-hidden="true">
        <span className="skeleton-line skeleton-line--short" />
        <span className="skeleton-line skeleton-line--title" />
        <span className="skeleton-line skeleton-line--meta" />
        <span className="skeleton-line skeleton-line--button" />
        <span className="skeleton-line" />
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

/** Lazy 16:9 still for one episode; the URL is only resolved near the viewport. */
function EpisodeStill({ episode, progress }: { episode: EmbyLibraryItem; progress: number }) {
  const [src, setSrc] = useState<string | null>(null);
  const [failed, setFailed] = useState(false);
  const ref = useRef<HTMLSpanElement>(null);
  const tag = episode.imageTags.Thumb;

  useEffect(() => {
    setSrc(null);
    setFailed(false);
    if (!tag) return undefined;
    const node = ref.current;
    if (!node) return undefined;
    let disposed = false;
    const load = () => {
      void safeImageUrl(episode, "Thumb", 320).then((url) => {
        if (!disposed) setSrc(url);
      });
    };
    if (typeof IntersectionObserver === "undefined") {
      load();
      return () => {
        disposed = true;
      };
    }
    // Resolve one bridge URL per row, only when it approaches the viewport.
    const observer = new IntersectionObserver(
      (entries) => {
        if (!entries.some((entry) => entry.isIntersecting)) return;
        observer.disconnect();
        load();
      },
      { rootMargin: "320px 0px" },
    );
    observer.observe(node);
    return () => {
      disposed = true;
      observer.disconnect();
    };
  }, [episode.id, tag]);

  return (
    <span className="episode-row__still" ref={ref} aria-hidden="true">
      {src && !failed ? (
        <>
          <img
            src={src}
            alt=""
            loading="lazy"
            decoding="async"
            onError={() => setFailed(true)}
          />
          <span className="episode-row__still-index">{episode.indexNumber ?? "·"}</span>
        </>
      ) : (
        <span className="episode-row__still-fallback">{episode.indexNumber ?? "·"}</span>
      )}
      <span className="episode-row__still-play">
        <Icon name="play" size={17} />
      </span>
      {progress > 0 && <ProgressBar value={progress} className="episode-row__still-progress" />}
    </span>
  );
}

function EpisodeSection({
  episodes,
  seriesName,
  focusEpisodeId,
  nextEpisodeId,
  pendingId,
  playbackPending,
  onPlay,
  onTogglePlayed,
}: {
  episodes: EmbyLibraryItem[];
  seriesName: string;
  focusEpisodeId: string | null;
  nextEpisodeId: string | null;
  pendingId: string | null;
  playbackPending: boolean;
  onPlay: (episode: EmbyLibraryItem) => void;
  onTogglePlayed: (episode: EmbyLibraryItem, played: boolean) => Promise<void>;
}) {
  const groups = useMemo(() => groupSeasons(episodes), [episodes]);
  const [selected, setSelected] = useState<string | null>(null);
  const [watchedOverrides, setWatchedOverrides] = useState<Record<string, boolean>>({});
  const [pendingWatched, setPendingWatched] = useState<string | null>(null);
  const [watchedError, setWatchedError] = useState<string | null>(null);

  // Keep an optimistic mark only until the refreshed episode list agrees.
  useEffect(() => {
    setWatchedOverrides((current) => {
      let changed = false;
      const next = { ...current };
      for (const [id, played] of Object.entries(next)) {
        const episode = episodes.find((entry) => entry.id === id);
        if (episode?.userData.played === played) {
          delete next[id];
          changed = true;
        }
      }
      return changed ? next : current;
    });
  }, [episodes]);

  const toggleEpisodeWatched = async (episode: EmbyLibraryItem, played: boolean) => {
    if (pendingWatched) return;
    const next = !played;
    setPendingWatched(episode.id);
    setWatchedError(null);
    setWatchedOverrides((current) => ({ ...current, [episode.id]: next }));
    try {
      await onTogglePlayed(episode, next);
    } catch (reason) {
      setWatchedOverrides((current) => {
        const copy = { ...current };
        delete copy[episode.id];
        return copy;
      });
      setWatchedError(describeEmbyError(reason, "更新观看状态失败，请稍后重试。"));
    } finally {
      setPendingWatched(null);
    }
  };

  // Return from the player: open the season that holds the episode on screen.
  const focusSeasonKey = useMemo(() => {
    if (!focusEpisodeId) return null;
    const group = groups.find((candidate) =>
      candidate.episodes.some((episode) => episode.id === focusEpisodeId),
    );
    return group?.key ?? null;
  }, [focusEpisodeId, groups]);

  // Open the season that holds the next episode so its row is visible without
  // hunting through the season tabs.
  const nextGroupKey = useMemo(() => {
    if (!nextEpisodeId) return null;
    return (
      groups.find((group) => group.episodes.some((episode) => episode.id === nextEpisodeId))
        ?.key ?? null
    );
  }, [groups, nextEpisodeId]);

  useEffect(() => {
    if (focusSeasonKey) setSelected(focusSeasonKey);
  }, [focusSeasonKey]);

  const activeKey = groups.some((group) => group.key === selected)
    ? selected
    : focusSeasonKey ?? nextGroupKey ?? defaultSeason(groups);
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
      {watchedError && <ErrorNotice message={watchedError} />}
      <ol className="episode-list" aria-label={active.label}>
        {active.episodes.map((episode) => {
          const progress = itemProgress(episode);
          const pending = pendingId === episode.id;
          const focused = focusEpisodeId === episode.id;
          const played = watchedOverrides[episode.id] ?? episode.userData.played;
          const watchPending = pendingWatched === episode.id;
          const label = itemEpisodeLabel(episode) ?? "";
          const meta = [label, formatRuntimeTicks(episode.runTimeTicks)].filter(
            (part) => part && part !== "时长未知",
          );
          return (
            <li
              key={episode.id}
              className={[
                "episode-list__item",
                focused ? "episode-list__item--focused" : "",
                nextEpisodeId === episode.id ? "episode-list__item--next" : "",
              ]
                .filter(Boolean)
                .join(" ")}
            >
              <button
                type="button"
                className={`episode-row${pending ? " is-pending" : ""}`}
                disabled={playbackPending}
                aria-current={focused ? "true" : undefined}
                aria-label={`播放《${seriesName}》${label} ${episode.name}`}
                onClick={() => onPlay(episode)}
              >
                <EpisodeStill episode={episode} progress={progress} />
                <span className="episode-row__body">
                  <span className="episode-row__title">{episode.name}</span>
                  <span className="episode-row__meta">{meta.join(" · ")}</span>
                  {episode.overview && <span className="episode-row__overview">{episode.overview}</span>}
                </span>
                <span className="episode-row__state">
                  {pending ? (
                    <span className="spinner spinner--small" aria-label="正在准备" />
                  ) : progress > 0 && !played ? (
                    `${Math.round(progress * 100)}%`
                  ) : null}
                </span>
              </button>
              <button
                type="button"
                className={`episode-watch${played ? " is-watched" : ""}`}
                aria-label={`${played ? "标记为未看完" : "标记为已看完"}：${seriesName} ${label} ${episode.name}`}
                aria-pressed={played}
                title={played ? "已看完，点击标记为未看完" : "未看完，点击标记为已看完"}
                disabled={pendingWatched !== null}
                onClick={() => void toggleEpisodeWatched(episode, played)}
              >
                {watchPending ? (
                  <span className="spinner spinner--small" aria-hidden="true" />
                ) : (
                  <Icon name="check" size={14} />
                )}
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
  focusEpisodeId = null,
  session,
  desktop,
  onBack,
  onPlay,
  onOpenItem,
  onOpenPerson,
}: DetailScreenProps) {
  const detail = useEmbyDetail(itemId, seed, desktop && Boolean(session));
  const playback = usePlaybackLauncher();
  const headingRef = useRef<HTMLHeadingElement>(null);
  const [pendingEpisodeId, setPendingEpisodeId] = useState<string | null>(null);
  const [watchedOverride, setWatchedOverride] = useState<boolean | null>(null);
  const [pendingAction, setPendingAction] = useState<"played" | "favorite" | null>(null);
  const [actionError, setActionError] = useState<string | null>(null);

  const item = detail.item;
  const effectiveWatched = watchedOverride ?? item?.userData.played ?? false;
  const favoriteState = item?.userData.favorite === true;
  const isSeries = item?.type === "Series";
  const episodes = useEmbyEpisodes(isSeries ? item.id : null, desktop && Boolean(session));
  /**
   * Series page's primary action, Plex/Emby style: resume the episode with a
   * saved position, otherwise the first unplayed one, otherwise start over.
   * The full episode list is already loaded, so no extra request is needed.
   */
  const nextEpisode = useMemo(() => {
    if (!isSeries || episodes.items.length === 0) return null;
    // The episode a Continue-watching card came from wins over the first
    // in-progress episode, so its progress and season stay on screen.
    const focused = focusEpisodeId
      ? episodes.items.find((entry) => entry.id === focusEpisodeId)
      : null;
    if (focused && !focused.userData.played && itemProgress(focused) > 0) {
      return { episode: focused, mode: "resume" as const };
    }
    const inProgress = episodes.items.find(
      (entry) => !entry.userData.played && itemProgress(entry) > 0,
    );
    if (inProgress) return { episode: inProgress, mode: "resume" as const };
    const firstUnplayed = episodes.items.find((entry) => !entry.userData.played);
    if (firstUnplayed) return { episode: firstUnplayed, mode: "play" as const };
    return { episode: episodes.items[0], mode: "restart" as const };
  }, [episodes.items, focusEpisodeId, isSeries]);

  // A series itself has no streams: its next episode's detail supplies the
  // audio/subtitle choices for the series-level (per-language) preference.
  const trackEpisodeId = isSeries ? nextEpisode?.episode.id ?? null : null;
  const trackSource = useAsyncResource<EmbyItem | null>(
    () => (trackEpisodeId ? embyItem(trackEpisodeId) : Promise.resolve(null)),
    `detail-streams:${trackEpisodeId ?? ""}`,
    { enabled: Boolean(trackEpisodeId) && desktop && Boolean(session) },
  );
  const similar = useEmbySimilar(
    item && (item.type === "Movie" || item.type === "Series") ? item.id : null,
    desktop && Boolean(session),
  );

  const focusedItemRef = useRef<string | null>(null);
  useEffect(() => {
    // Wait for the requested item: during an item change the previous detail
    // may still be rendered until its replacement arrives.
    if (item?.id === itemId && focusedItemRef.current !== itemId) {
      focusedItemRef.current = itemId;
      headingRef.current?.focus({ preventScroll: true });
    }
  }, [item, itemId]);

  // Local toggles belong to one detail item; drop them when the page changes.
  useEffect(() => {
    setWatchedOverride(null);
    setPendingAction(null);
    setActionError(null);
  }, [itemId]);

  useEscape(onBack);

  const startPlayback = async () => {
    if (!item || playback.pending || !isPlayableItemType(item.type)) return;
    const target = await playback.launch(item.id, itemStartSeconds(item));
    if (target) {
      onPlay(target, playbackMeta(item, target.chapters), [item], item.id);
    }
  };

  const playEpisode = async (episode: EmbyLibraryItem) => {
    if (!item || playback.pending) return;
    setPendingEpisodeId(episode.id);
    const target = await playback.launch(episode.id, itemStartSeconds(episode));
    setPendingEpisodeId(null);
    if (!target) return;
    // Episodes after this one follow automatically; track choice is per series.
    const list = episodes.items.length > 0 ? episodes.items : [episode];
    const index = list.findIndex((entry) => entry.id === episode.id);
    const queue = index >= 0 ? list.slice(index) : [episode];
    onPlay(target, playbackMeta(episode, target.chapters), queue, item.id);
  };

  const toggleEpisodeWatched = async (episode: EmbyLibraryItem, played: boolean) => {
    await embySetPlayed(episode.id, played);
    // The series' aggregate progress, the header count and the next-up target
    // all derive from these two reads.
    setWatchedOverride(null);
    episodes.reload();
    detail.reload();
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
  const progressItem = isSeries && nextEpisode?.mode === "resume" ? nextEpisode.episode : item;
  const progress = itemProgress(progressItem);
  const runtime = formatRuntimeTicks(item.runTimeTicks);
  const seasonCount = isSeries ? groupSeasons(episodes.items).length : 0;

  // Episode pages read like Plex Web: series, season, "第 8 集 · name".
  const seasonLabel =
    item.parentIndexNumber === null ? null : item.parentIndexNumber === 0 ? "特别篇" : `第 ${item.parentIndexNumber} 季`;
  const episodeLine = [episode ? compactEpisodeLabel(item) : "", item.name].filter(Boolean).join(" · ");

  const metaParts: string[] = [];
  const year = itemYearLabel(item);
  if (year) metaParts.push(year);
  if (runtime !== "时长未知") metaParts.push(runtime);
  if (isSeries && episodes.items.length > 0) {
    metaParts.push(`${seasonCount} 季`, `${episodes.items.length} 集`);
  }
  if (item.genres.length > 0) metaParts.push(item.genres.slice(0, 2).join(" / "));

  const remaining = formatRemainingTicks(progressItem);
  const rating = formatCommunityRating(item.communityRating);
  const displayTitle = episode && item.seriesName ? item.seriesName : item.name;
  const progressLabel =
    progressItem.type === "Episode" && !episode
      ? [compactEpisodeLabel(progressItem), progressItem.name].filter(Boolean).join(" · ")
      : "上次看到";
  const mediaKicker = episode
    ? ["剧集", seasonLabel].filter(Boolean).join(" · ")
    : isSeries
      ? "剧集"
      : item.type === "Movie"
        ? "电影"
        : "视频";
  const cast = castAndCrew(item.people);
  const supportsWatchState = isSeries || playable;
  const trackStreams = isSeries ? trackSource.data?.mediaStreams ?? [] : item.mediaStreams;

  const toggleWatched = async () => {
    if (pendingAction) return;
    const next = !effectiveWatched;
    setPendingAction("played");
    setActionError(null);
    try {
      await embySetPlayed(item.id, next);
      setWatchedOverride(next);
      // A Series toggle marks the whole season on the server, so the episode
      // rows, the season tab counts and the "continue playing" target are all
      // stale too: re-read both.
      detail.reload();
      if (isSeries) episodes.reload();
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
      // Re-read the item so the button reflects the server's UserData.Favorite.
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

      <section className="detail-cinema" aria-labelledby="detail-title" aria-busy={detail.loading}>
        <MediaArtwork
          className="detail-cinema__art"
          src={item.backdropUrl ?? item.thumbUrl ?? item.posterUrl}
          alt=""
          title={displayTitle}
          loading="eager"
        />
        <div className="detail-cinema__scrim" aria-hidden="true" />
        <div className="detail-cinema__content">
          <p className="detail-kicker">
            <span className="detail-kicker__dot" aria-hidden="true" />
            {mediaKicker}
          </p>
          <h1 id="detail-title" className="detail-title" ref={headingRef} tabIndex={-1}>
            {displayTitle}
          </h1>
          {episode && item.seriesName && <p className="detail-subtitle">{episodeLine}</p>}
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

          {progress > 0 && (
            <div className="detail-progress">
              <ProgressBar value={progress} className="detail-progress__bar" />
              <div className="detail-progress__copy">
                <span>{progressLabel}</span>
                <span>{remaining ?? `已观看 ${Math.round(progress * 100)}%`}</span>
              </div>
            </div>
          )}

          <div className="detail-actions">
            {isSeries && nextEpisode && (
              <button
                className="button button--primary button--large"
                type="button"
                onClick={() => void playEpisode(nextEpisode.episode)}
                disabled={playback.pending || Boolean(pendingEpisodeId) || episodes.loading}
              >
                {playback.pending || pendingEpisodeId ? (
                  <span className="spinner spinner--small spinner--dark" aria-hidden="true" />
                ) : (
                  <Icon name="play" size={18} />
                )}
                {episodes.loading
                  ? "正在读取剧集…"
                  : `${nextEpisode.mode === "resume" ? "继续播放" : nextEpisode.mode === "restart" ? "重新播放" : "播放"} ${compactEpisodeLabel(nextEpisode.episode)}`}
              </button>
            )}
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
                    <Icon name="check" size={19} />
                  )}
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
                    <HeartIcon filled={favoriteState === true} size={19} />
                  )}
                </button>
              </>
            )}
            {item.type === "MusicAlbum" && <span className="detail-hint">音乐专辑暂不支持播放。</span>}
          </div>

          <TrackPickers scope={item.id} streams={trackStreams} />

          {playback.error && <ErrorNotice message={playback.error} />}
          {actionError && <ErrorNotice message={actionError} />}
        </div>
      </section>

      <div className="detail-body">
        {isSeries && (
          <section className="episodes" aria-labelledby="episodes-title">
            <div className="section-header">
              <h2 className="section-title" id="episodes-title">
                剧集
              </h2>
              {episodes.items.length > 0 && (
                <span className="section-summary">
                  {episodes.items.length} 集 · 已看 {episodes.items.filter((entry) => entry.userData.played).length}
                </span>
              )}
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
                key={item.id}
                episodes={episodes.items}
                seriesName={item.name}
                focusEpisodeId={focusEpisodeId ?? null}
                nextEpisodeId={nextEpisode && nextEpisode.mode !== "restart" ? nextEpisode.episode.id : null}
                pendingId={pendingEpisodeId}
                playbackPending={playback.pending}
                onPlay={(next) => void playEpisode(next)}
                onTogglePlayed={toggleEpisodeWatched}
              />
            )}
          </section>
        )}

        <section className="detail-about" aria-label="简介">
          {item.tagline && <p className="detail-tagline">{item.tagline}</p>}
          <p className="detail-overview">{item.overview?.trim() || "暂无简介。"}</p>
        </section>

        <DetailFacts item={item} />

        {(cast.length > 0 || similar.items.length > 0) && (
          <div className="detail-rows">
            {cast.length > 0 && (
              <HubRow title="演职人员" variant="person">
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
              <HubRow title="更多类似内容" variant="poster">
                {similar.items.map((other) => (
                  <PosterCard key={other.id} item={other} landscape={false} onSelect={onOpenItem} />
                ))}
              </HubRow>
            )}
          </div>
        )}
      </div>
    </main>
  );
}
