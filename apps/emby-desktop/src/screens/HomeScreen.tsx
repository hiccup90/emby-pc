import type { ReactNode } from "react";
import type { EmbyView } from "../emby/contract";
import { embyItems } from "../emby/client";
import { decorateItems } from "../emby/artwork";
import { includeItemTypesForView } from "../emby/library";
import { useAsyncResource } from "../emby/useAsyncResource";
import type { EmbyLibraryItem, EmbyLoadStatus } from "../emby/useEmby";
import { ContinueCard, HubRow, PosterCard } from "./cards";
import "./HomeScreen.css";

/** Number of newest items shown in each home library row. */
const HOME_ROW_LIMIT = 8;

export interface HomeScreenProps {
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
  status,
  error,
  onRetry,
  children,
}: {
  title: string;
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
    <HubRow title={title} variant="landscape">
      {children}
    </HubRow>
  );
}

/** One selected library's fixed-size "recently added" row. */
function HomeLibrarySection({
  view,
  userId,
  onOpen,
}: {
  view: EmbyView;
  userId: string;
  onOpen: (item: EmbyLibraryItem) => void;
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
    `home-library:${userId}:${view.id}:${HOME_ROW_LIMIT}`,
    {
      enabled: Boolean(userId),
      fallbackError: `无法读取「${view.name}」的最近入库。`,
    },
  );
  const items = resource.data ?? [];

  if (!userId) return null;
  if (resource.status === "ready" && items.length === 0) return null;

  return (
    <section className="home-hub" aria-label={`${view.name} 最近入库`}>
      <div className="hub__header">
        <h2 className="hub__title">{view.name}</h2>
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
  resume,
  resumeStatus,
  resumeError,
  views,
  userId,
  onOpen,
  onPlay,
  onSeeAll,
  onRetryResume,
}: HomeScreenProps) {
  const nothingLoaded = resumeStatus === "ready" && resume.length === 0 && views.length === 0;

  return (
    <main className="page home-page">
      {!(resumeStatus === "ready" && resume.length === 0) && (
        <HomeSection
          title="继续观看"
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
        <HomeLibrarySection key={view.id} view={view} userId={userId} onOpen={onOpen} />
      ))}

      {nothingLoaded && (
        <p className="empty-state home-page__empty">
          还没有可以展示的内容，
          <button type="button" className="home-page__link" onClick={onSeeAll}>
            去媒体库看看
          </button>
          。
        </p>
      )}
    </main>
  );
}
