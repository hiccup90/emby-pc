import type { ReactNode } from "react";
import type { EmbyLibraryItem, EmbyLoadStatus } from "../emby/useEmby";
import { Icon } from "./shared";
import { ContinueCard, HubRow, PosterCard } from "./cards";
import "./HomeScreen.css";

export interface HomeScreenProps {
  /** Continue-watching items (server order, usually most recent first). */
  resume: EmbyLibraryItem[];
  resumeStatus: EmbyLoadStatus;
  resumeError?: string | null;
  /** Recently added items for the poster row (server-sorted, newest first). */
  recent: EmbyLibraryItem[];
  recentStatus: EmbyLoadStatus;
  recentError?: string | null;
  onOpen: (item: EmbyLibraryItem) => void;
  onPlay: (item: EmbyLibraryItem) => void;
  /** Jump to the full library view ("查看全部"). */
  onSeeAll: () => void;
  /** Retry the recent row after a failure. */
  onRetryRecent?: () => void;
  /** Retry the continue-watching row after a failure. */
  onRetryResume?: () => void;
}

function HomeSkeletonRow({ landscape, count }: { landscape: boolean; count: number }) {
  return (
    <div
      className={`hub__track${landscape ? "" : " hub__track--poster"} home-skeleton`}
      aria-hidden="true"
    >
      {Array.from({ length: count }, (_, index) => (
        <span className="skeleton-card" key={index}>
          <span className={`skeleton-block${landscape ? " skeleton-block--landscape" : ""}`} />
          <span className="skeleton-line" />
          <span className="skeleton-line skeleton-line--short" />
        </span>
      ))}
    </div>
  );
}

function HomeSection({
  title,
  count,
  variant,
  action,
  status,
  error,
  onRetry,
  emptyLabel,
  skeletonCount,
  children,
}: {
  title: string;
  count: number;
  variant: "landscape" | "poster";
  action?: ReactNode;
  status: EmbyLoadStatus;
  error?: string | null;
  onRetry?: () => void;
  emptyLabel: string;
  skeletonCount: number;
  children: ReactNode;
}) {
  if (status === "loading" || status === "idle") {
    return (
      <section className="home-hub" aria-label={title} aria-busy="true">
        <div className="hub__header">
          <h2 className="hub__title">{title}</h2>
        </div>
        <HomeSkeletonRow landscape={variant === "landscape"} count={skeletonCount} />
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

  if (count === 0) {
    return (
      <section className="home-hub" aria-label={title}>
        <div className="hub__header">
          <h2 className="hub__title">{title}</h2>
        </div>
        <p className="home-hub__empty">{emptyLabel}</p>
      </section>
    );
  }

  return (
    <HubRow title={title} count={count} variant={variant} action={action}>
      {children}
    </HubRow>
  );
}

/**
 * Home page: "继续观看" (resumes playback on click) and "最近添加" (posters that
 * open the detail page). The data layer supplies the load state, so a pending
 * request never renders as an empty result.
 */
export default function HomeScreen({
  resume,
  resumeStatus,
  resumeError,
  recent,
  recentStatus,
  recentError,
  onOpen,
  onPlay,
  onSeeAll,
  onRetryRecent,
  onRetryResume,
}: HomeScreenProps) {
  const loading = resumeStatus === "loading" || recentStatus === "loading";
  const failed = resumeStatus === "error" && recentStatus === "error";
  const nothingLoaded =
    !loading &&
    !failed &&
    resume.length === 0 &&
    recent.length === 0 &&
    resumeStatus === "ready" &&
    recentStatus === "ready";

  return (
    <main className="page home-page">
      <header className="home-page__header">
        <div>
          <h1 className="page-title">首页</h1>
          <p className="home-page__subtitle">继续上次的进度，或看看最近入库的新内容。</p>
        </div>
        <button type="button" className="button button--secondary" onClick={onSeeAll}>
          浏览媒体库
        </button>
      </header>

      <HomeSection
        title="继续观看"
        count={resume.length}
        variant="landscape"
        status={resumeStatus}
        error={resumeError}
        onRetry={onRetryResume}
        emptyLabel="还没有正在观看的内容。"
        skeletonCount={4}
      >
        {resume.map((item) => (
          <ContinueCard key={item.id} item={item} onPlay={onPlay} />
        ))}
      </HomeSection>

      <HomeSection
        title="最近添加"
        count={recent.length}
        variant="poster"
        status={recentStatus}
        error={recentError}
        onRetry={onRetryRecent}
        emptyLabel="媒体库中还没有新内容。"
        skeletonCount={8}
        action={
          recentStatus === "ready" && recent.length > 0 ? (
            <button type="button" className="home-hub__see-all" onClick={onSeeAll}>
              查看全部
              <Icon name="chevron-right" size={14} />
            </button>
          ) : undefined
        }
      >
        {recent.map((item) => (
          <PosterCard key={item.id} item={item} landscape={false} onSelect={onOpen} />
        ))}
      </HomeSection>

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
