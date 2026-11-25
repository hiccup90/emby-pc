import { useCallback, useEffect, useLayoutEffect, useRef, useState } from "react";
import type { MouseEvent as ReactMouseEvent, ReactNode, RefObject } from "react";
import type { EmbyView } from "../emby/contract";
import { embyItems } from "../emby/client";
import { decorateItems } from "../emby/artwork";
import { includeItemTypesForView } from "../emby/library";
import {
  formatCommunityRating,
  formatRemainingTicks,
  formatRuntimeTicks,
  itemProgress,
  itemYearLabel,
} from "../emby/format";
import { useAsyncResource } from "../emby/useAsyncResource";
import type { EmbyLibraryItem, EmbyLoadStatus } from "../emby/useEmby";
import { ContinueCard, HubRow, PosterCard } from "./cards";
import { useItemActions } from "./ItemActions";
import { Icon, MediaArtwork, ProgressBar } from "./shared";
import "./HomeScreen.css";

/** Fetch enough for a wide window; the grid only renders what fits. */
const HOME_ROW_FETCH = 16;
/** Loading placeholder count; the real row is sized after its first layout. */
const HOME_ROW_SKELETON = 8;

/** Whether a library row has data, is empty, or is still deciding. */
type HomeRowState = "pending" | "empty" | "filled";

/** Short "S1E8" label for the hero's secondary line. */
function compactEpisodeLabel(item: EmbyLibraryItem): string {
  const { parentIndexNumber, indexNumber } = item;
  if (parentIndexNumber === null || indexNumber === null) return "";
  return `S${parentIndexNumber}E${indexNumber}`;
}

/**
 * The first continue-watching item, blown up into a full-width still. It is
 * the same item the old continue row put first, so the page opens on what the
 * user was already watching instead of a generic heading.
 */
function HomeHero({
  item,
  onOpen,
  onPlay,
}: {
  item: EmbyLibraryItem;
  onOpen: (item: EmbyLibraryItem) => void;
  onPlay: (item: EmbyLibraryItem) => void;
}) {
  const actions = useItemActions();
  const episode = item.type === "Episode";
  const episodeLabel = episode ? compactEpisodeLabel(item) : "";
  const title = episode && item.seriesName ? item.seriesName : item.name;
  const subtitle = episode
    ? [episodeLabel, item.name].filter(Boolean).join(" · ")
    : null;
  const progress = itemProgress(item);
  const remaining = formatRemainingTicks(item);
  const runtime = formatRuntimeTicks(item.runTimeTicks);
  const rating = formatCommunityRating(item.communityRating);
  const meta = [
    itemYearLabel(item),
    item.genres.slice(0, 2).join(" / "),
    runtime !== "时长未知" && item.type !== "Series" ? runtime : null,
  ].filter((part): part is string => Boolean(part));

  const openMenu = (event: ReactMouseEvent<HTMLButtonElement>) => {
    if (!actions) return;
    const rect = event.currentTarget.getBoundingClientRect();
    actions.openMenu(item, rect.left, rect.bottom + 8);
  };

  return (
    <section className="home-hero" aria-labelledby="home-hero-title">
      <MediaArtwork
        className="home-hero__art"
        src={item.backdropUrl ?? item.thumbUrl ?? item.posterUrl}
        alt=""
        title={title}
        loading="eager"
      />
      <div className="home-hero__scrim" aria-hidden="true" />
      <div className="home-hero__content">
        <p className="home-hero__kicker">
          <span className="home-hero__kicker-dot" aria-hidden="true" />
          继续观看
        </p>
        <h1 className="home-hero__title" id="home-hero-title">
          {title}
        </h1>
        {subtitle && <p className="home-hero__subtitle">{subtitle}</p>}
        {(meta.length > 0 || rating || item.officialRating) && (
          <p className="home-hero__meta">
            {meta.length > 0 && <span>{meta.join(" · ")}</span>}
            {rating && (
              <span className="home-hero__rating">
                <span aria-hidden="true">★</span> {rating}
              </span>
            )}
            {item.officialRating && <span className="home-hero__cert">{item.officialRating}</span>}
          </p>
        )}
        {progress > 0 && (
          <div className="home-hero__progress">
            <ProgressBar value={progress} className="home-hero__bar" />
            <div className="home-hero__progress-copy">
              <span>{episode ? `已看 ${Math.round(progress * 100)}%` : "上次看到"}</span>
              {remaining && <span>{remaining}</span>}
            </div>
          </div>
        )}
        <div className="home-hero__actions">
          <button
            type="button"
            className="button button--primary button--large home-hero__play"
            onClick={() => onPlay(item)}
          >
            <Icon name="play" size={18} />
            {progress > 0 ? "继续播放" : "播放"}
          </button>
          <button
            type="button"
            className="home-hero__icon"
            onClick={() => onOpen(item)}
            aria-label={`查看《${title}》详情`}
            title="查看详情"
          >
            <Icon name="info" size={20} />
          </button>
          {actions && (
            <button
              type="button"
              className="home-hero__icon"
              onClick={openMenu}
              aria-label={`更多《${title}》操作`}
              title="更多操作"
            >
              <Icon name="more" size={20} />
            </button>
          )}
        </div>
      </div>
    </section>
  );
}

/** Keeps the hero's height while continue-watching is still loading. */
function HomeHeroSkeleton() {
  return (
    <div className="home-hero home-hero--skeleton" aria-hidden="true">
      <div className="home-hero__content">
        <span className="skeleton-line skeleton-line--short" />
        <span className="skeleton-line skeleton-line--title" />
        <span className="skeleton-line skeleton-line--meta" />
        <span className="skeleton-line skeleton-line--button" />
      </div>
    </div>
  );
}

export interface HomeScreenProps {
  /** Changes after delete/identify so every library row re-reads its items. */
  refreshKey?: number;
  /** Continue-watching items (server order, usually most recent first). */
  resume: EmbyLibraryItem[];
  resumeStatus: EmbyLoadStatus;
  resumeError?: string | null;
  /** Libraries selected for the home page, already in display order. */
  views: EmbyView[];
  /** Resets each library row when the signed-in user changes. */
  userId: string;
  onOpen: (item: EmbyLibraryItem) => void;
  onPlay: (item: EmbyLibraryItem) => void;
  /** Jump to the full library view when the home page is empty. */
  onSeeAll: () => void;
  /** Open one library's full grid from its home row header. */
  onSeeLibrary: (viewId: string) => void;
  /** Retry the continue-watching row after a failure. */
  onRetryResume?: () => void;
}

function HomeSkeletonRow({ count }: { count: number }) {
  return (
    <div className="hub__track home-skeleton" aria-hidden="true">
      {Array.from({ length: count }, (_, index) => (
        <span className="skeleton-card" key={index}>
          <span className="skeleton-block skeleton-block--landscape" />
          <span className="skeleton-line" />
          <span className="skeleton-line skeleton-line--short" />
        </span>
      ))}
    </div>
  );
}

function HomeLibrarySkeleton() {
  const ref = useRef<HTMLDivElement>(null);
  const columns = useGridColumns(ref);

  return (
    <div className="home-library-grid home-skeleton" ref={ref} aria-hidden="true">
      {Array.from({ length: columns }, (_, index) => (
        <span className="skeleton-card" key={index}>
          <span className="skeleton-block" />
          <span className="skeleton-line" />
          <span className="skeleton-line skeleton-line--short" />
        </span>
      ))}
    </div>
  );
}

/** How many fixed-width poster columns the grid currently fits. */
function useGridColumns(ref: RefObject<HTMLDivElement | null>): number {
  const [columns, setColumns] = useState(HOME_ROW_SKELETON);

  useLayoutEffect(() => {
    const grid = ref.current;
    if (!grid) return undefined;
    const update = () => {
      // auto-fill exposes one computed track per column, including empty ones,
      // so this is exactly how many cards fit on one line at the current width.
      const tracks = getComputedStyle(grid).gridTemplateColumns.trim().split(/\s+/).length;
      setColumns((current) => (current === tracks ? current : tracks));
    };
    update();
    const observer = new ResizeObserver(update);
    observer.observe(grid);
    return () => observer.disconnect();
  }, [ref]);

  return columns;
}

/** One library row: fixed-size posters, as many as the window fits on one line. */
function HomeLibraryGrid({
  items,
  onOpen,
}: {
  items: EmbyLibraryItem[];
  onOpen: (item: EmbyLibraryItem) => void;
}) {
  const ref = useRef<HTMLDivElement>(null);
  const columns = useGridColumns(ref);

  return (
    <div className="home-library-grid" ref={ref}>
      {items.slice(0, columns).map((item) => (
        <PosterCard key={item.id} item={item} landscape={false} onSelect={onOpen} />
      ))}
    </div>
  );
}

function HomeSection({
  title,
  meta,
  status,
  error,
  onRetry,
  children,
}: {
  title: string;
  meta?: ReactNode;
  status: EmbyLoadStatus;
  error?: string | null;
  onRetry?: () => void;
  children: ReactNode;
}) {
  if (status === "loading" || status === "idle") {
    return (
      <section className="home-hub" aria-label={title} aria-busy="true">
        <div className="hub__header">
          <h2 className="hub__title">{title}</h2>
        </div>
        <HomeSkeletonRow count={4} />
      </section>
    );
  }

  if (status === "error") {
    return (
      <section className="home-hub" aria-label={title}>
        <div className="hub__header">
          <h2 className="hub__title">{title}</h2>
        </div>
        <p className="home-hub__error" role="status">
          {error ?? `无法读取「${title}」。`}
          {onRetry && (
            <button type="button" className="home-hub__retry" onClick={onRetry}>
              重试
            </button>
          )}
        </p>
      </section>
    );
  }

  return (
    <HubRow title={title} meta={meta} variant="landscape">
      {children}
    </HubRow>
  );
}

/** One selected library's fixed-size "recently added" row. */
function HomeLibrarySection({
  view,
  userId,
  refreshKey,
  onOpen,
  onSeeLibrary,
  onRowState,
}: {
  view: EmbyView;
  userId: string;
  refreshKey: number;
  onOpen: (item: EmbyLibraryItem) => void;
  onSeeLibrary: (viewId: string) => void;
  onRowState: (viewId: string, state: HomeRowState) => void;
}) {
  const resource = useAsyncResource(
    () => {
      const includeItemTypes = includeItemTypesForView(view);
      return embyItems({
        parentId: view.id,
        recursive: true,
        sortBy: "DateCreated",
        sortOrder: "Descending",
        limit: HOME_ROW_FETCH,
        ...(includeItemTypes ? { includeItemTypes } : {}),
      }).then((page) => decorateItems(page.items));
    },
    `home-library:${userId}:${view.id}:${HOME_ROW_FETCH}:${refreshKey}`,
    {
      enabled: Boolean(userId),
      fallbackError: `无法读取「${view.name}」的最近入库。`,
    },
  );
  const items = resource.data ?? [];

  // A hidden empty row would otherwise leave the whole page blank when every
  // selected library is empty; the page decides the empty state from this.
  useEffect(() => {
    if (!userId) return;
    onRowState(
      view.id,
      resource.status === "ready" ? (items.length === 0 ? "empty" : "filled") : "pending",
    );
  }, [items.length, onRowState, resource.status, userId, view.id]);

  if (!userId) return null;
  if (resource.status === "ready" && items.length === 0) return null;

  return (
    <section className="home-hub" aria-label={`${view.name} 最近入库`}>
      <div className="hub__header">
        <div className="hub__heading">
          <h2 className="hub__title">{view.name}</h2>
          <span className="hub__meta">最近入库</span>
        </div>
        <button
          type="button"
          className="hub__more"
          onClick={() => onSeeLibrary(view.id)}
          aria-label={`查看「${view.name}」全部`}
        >
          查看全部
          <Icon name="chevron-right" size={14} />
        </button>
      </div>
      {resource.status === "loading" || resource.status === "idle" ? (
        <HomeLibrarySkeleton />
      ) : resource.status === "error" ? (
        <p className="home-hub__error" role="status">
          {resource.error ?? `无法读取「${view.name}」的最近入库。`}
          <button type="button" className="home-hub__retry" onClick={resource.reload}>
            重试
          </button>
        </p>
      ) : (
        <HomeLibraryGrid items={items} onOpen={onOpen} />
      )}
    </section>
  );
}

/**
 * Home page: "继续观看" followed by one fixed-size "recently added" row per
 * selected Emby library. The rows never scroll horizontally; narrow windows
 * wrap the cards instead.
 */
export default function HomeScreen({
  refreshKey = 0,
  resume,
  resumeStatus,
  resumeError,
  views,
  userId,
  onOpen,
  onPlay,
  onSeeAll,
  onSeeLibrary,
  onRetryResume,
}: HomeScreenProps) {
  const [rowStates, setRowStates] = useState<Record<string, HomeRowState>>({});
  const reportRowState = useCallback((viewId: string, state: HomeRowState) => {
    setRowStates((current) =>
      current[viewId] === state ? current : { ...current, [viewId]: state },
    );
  }, []);
  const allRowsEmpty =
    views.length > 0 && views.every((view) => rowStates[view.id] === "empty");
  // Only after continue-watching has answered too: otherwise the panel would
  // flash under its skeleton while the rows are already known to be empty.
  const empty =
    resumeStatus === "ready" &&
    resume.length === 0 &&
    (views.length === 0 || allRowsEmpty);
  // The hero is the first continue-watching entry. Removing it from the row
  // below keeps the same title from appearing twice on one screen.
  const hero = resume[0] ?? null;
  const resumeRest = hero ? resume.slice(1) : resume;
  const showResumeRow = resumeRest.length > 0 || resumeStatus === "error";

  return (
    <main className="page home-page">
      {hero ? (
        <HomeHero item={hero} onOpen={onOpen} onPlay={onPlay} />
      ) : resumeStatus === "loading" || resumeStatus === "idle" ? (
        <HomeHeroSkeleton />
      ) : null}
      {showResumeRow && (
        <HomeSection
          title="继续观看"
          meta={resume.length > 0 ? `${resume.length} 部未看完` : undefined}
          status={resumeStatus}
          error={resumeError}
          onRetry={onRetryResume}
        >
          {resumeRest.map((item) => (
            <ContinueCard key={item.id} item={item} onPlay={onPlay} onDetails={onOpen} />
          ))}
        </HomeSection>
      )}

      {views.map((view) => (
        <HomeLibrarySection
          key={view.id}
          view={view}
          userId={userId}
          refreshKey={refreshKey}
          onOpen={onOpen}
          onSeeLibrary={onSeeLibrary}
          onRowState={reportRowState}
        />
      ))}

      {empty && (
        <div className="home-page__empty">
          <span className="home-page__empty-icon" aria-hidden="true">
            <Icon name="film" size={26} />
          </span>
          <p>{views.length > 0 ? "选中的媒体库还没有内容" : "还没有可以展示的内容"}</p>
          <button type="button" className="button button--secondary" onClick={onSeeAll}>
            去媒体库看看
          </button>
        </div>
      )}
    </main>
  );
}
