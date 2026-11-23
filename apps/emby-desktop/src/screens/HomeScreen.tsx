import { useCallback, useEffect, useState } from "react";
import type { ReactNode } from "react";
import type { EmbyView } from "../emby/contract";
import { embyItems } from "../emby/client";
import { decorateItems } from "../emby/artwork";
import { includeItemTypesForView } from "../emby/library";
import { useAsyncResource } from "../emby/useAsyncResource";
import type { EmbyLibraryItem, EmbyLoadStatus } from "../emby/useEmby";
import { ContinueCard, HubRow, PosterCard } from "./cards";
import { Icon } from "./shared";
import "./HomeScreen.css";

/** Number of newest items shown in each home library row. */
const HOME_ROW_LIMIT = 8;

/** Whether a library row has data, is empty, or is still deciding. */
type HomeRowState = "pending" | "empty" | "filled";

export interface HomeScreenProps {
  /** Changes after delete/identify so every library row re-reads its 8 items. */
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
  return (
    <div className="home-library-grid home-skeleton" aria-hidden="true">
      {Array.from({ length: HOME_ROW_LIMIT }, (_, index) => (
        <span className="skeleton-card" key={index}>
          <span className="skeleton-block" />
          <span className="skeleton-line" />
          <span className="skeleton-line skeleton-line--short" />
        </span>
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
        limit: HOME_ROW_LIMIT,
        ...(includeItemTypes ? { includeItemTypes } : {}),
      }).then((page) => decorateItems(page.items));
    },
    `home-library:${userId}:${view.id}:${HOME_ROW_LIMIT}:${refreshKey}`,
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
        <div className="home-library-grid">
          {items.map((item) => (
            <PosterCard key={item.id} item={item} landscape={false} onSelect={onOpen} />
          ))}
        </div>
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
  // A soft colour wash from the most recent in-progress title: the page gets a
  // sense of place without a header, and the image is already in the card row
  // (same URL), so it costs no extra request.
  const ambient =
    resume.find((item) => item.backdropUrl)?.backdropUrl ?? resume[0]?.posterUrl ?? null;

  return (
    <main className="page home-page">
      {ambient && (
        <div className="home-ambient" aria-hidden="true">
          <img src={ambient} alt="" decoding="async" />
        </div>
      )}
      {!(resumeStatus === "ready" && resume.length === 0) && (
        <HomeSection
          title="继续观看"
          meta={resume.length > 0 ? `${resume.length} 部未看完` : undefined}
          status={resumeStatus}
          error={resumeError}
          onRetry={onRetryResume}
        >
          {resume.map((item) => (
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
