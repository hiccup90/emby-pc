import { useEffect, useRef, useState } from "react";
import type { ReactNode } from "react";
import type { EmbyLibraryItem } from "../emby/useEmby";
import {
  formatRemainingTicks,
  itemEpisodeLabel,
  itemProgress,
  itemYearLabel,
} from "../emby/format";
import { Icon, MediaArtwork, ProgressBar } from "./shared";
import { PosterCard } from "./cards";
import "./HomeScreen.css";

export interface HomeScreenProps {
  /** Continue-watching items (server order, usually most recent first). */
  resume: EmbyLibraryItem[];
  /** Recently added items for the poster row. */
  recent: EmbyLibraryItem[];
  onOpen: (item: EmbyLibraryItem) => void;
  onPlay: (item: EmbyLibraryItem) => void;
  /** Jump to the full library view ("查看全部"). */
  onSeeAll: () => void;
}

/**
 * Simplified in-place copy of LibraryScreen's ContinueCard: same Plex look
 * (16:9 backdrop + amber progress bar) without importing a private component.
 */
function HomeResumeCard({
  item,
  onPlay,
}: {
  item: EmbyLibraryItem;
  onPlay: (item: EmbyLibraryItem) => void;
}) {
  const progress = itemProgress(item);
  const episode = item.type === "Episode";
  const title = episode && item.seriesName ? item.seriesName : item.name;
  const lead = episode ? itemEpisodeLabel(item) : itemYearLabel(item);
  const remaining = formatRemainingTicks(item);
  const meta = [lead, remaining].filter(Boolean).join(" · ");
  const label = episode
    ? `继续观看《${title}》${lead ? ` ${lead}` : ""} ${item.name}`
    : `继续观看《${title}》`;

  return (
    <button type="button" className="home-resume-card" onClick={() => onPlay(item)} aria-label={label}>
      <span className="home-resume-card__frame">
        <MediaArtwork src={item.backdropUrl ?? item.posterUrl} alt="" title={title} loading="lazy" />
        <ProgressBar value={progress} className="home-resume-card__progress" />
      </span>
      <span className="home-resume-card__title">{title}</span>
      <span className="home-resume-card__meta">{meta || "继续观看"}</span>
    </button>
  );
}

function HomeHub({
  title,
  count,
  variant,
  action,
  children,
}: {
  title: string;
  count: number;
  variant: "landscape" | "poster";
  action?: ReactNode;
  children: ReactNode;
}) {
  const trackRef = useRef<HTMLDivElement>(null);
  const [edges, setEdges] = useState({ start: true, end: true });

  useEffect(() => {
    const track = trackRef.current;
    if (!track) return undefined;
    const measure = () => {
      const max = track.scrollWidth - track.clientWidth;
      setEdges({ start: track.scrollLeft <= 2, end: track.scrollLeft >= max - 2 });
    };
    measure();
    track.addEventListener("scroll", measure, { passive: true });
    const observer = new ResizeObserver(measure);
    observer.observe(track);
    return () => {
      track.removeEventListener("scroll", measure);
      observer.disconnect();
    };
  }, [count]);

  const page = (direction: 1 | -1) => {
    const track = trackRef.current;
    if (!track) return;
    track.scrollBy({ left: direction * track.clientWidth * 0.9, behavior: "smooth" });
  };

  return (
    <section className="home-hub" aria-label={title}>
      <div className="home-hub__header">
        <div className="home-hub__heading">
          <h2 className="home-hub__title">{title}</h2>
          <span className="home-hub__count">{count} 项</span>
        </div>
        <div className="home-hub__actions">
          {action}
          {!(edges.start && edges.end) && (
            <div className="home-hub__nav">
              <button type="button" className="icon-button" onClick={() => page(-1)} disabled={edges.start} aria-label="向左滚动">
                <Icon name="chevron-left" size={18} />
              </button>
              <button type="button" className="icon-button" onClick={() => page(1)} disabled={edges.end} aria-label="向右滚动">
                <Icon name="chevron-right" size={18} />
              </button>
            </div>
          )}
        </div>
      </div>
      <div className={`home-hub__track home-hub__track--${variant}`} ref={trackRef}>
        {children}
      </div>
    </section>
  );
}

/**
 * Home page: two horizontal hubs, "继续观看" (resume playback on click) and
 * "最近添加" (posters opening the detail page). App decides where the data
 * comes from; this screen only renders it.
 */
export default function HomeScreen({ resume, recent, onOpen, onPlay, onSeeAll }: HomeScreenProps) {
  const empty = resume.length === 0 && recent.length === 0;

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

      {empty ? (
        <div className="page-message">
          <p className="empty-state">还没有可以展示的内容，去媒体库看看吧。</p>
          <button type="button" className="button button--primary" onClick={onSeeAll}>
            打开媒体库
          </button>
        </div>
      ) : (
        <>
          {resume.length > 0 && (
            <HomeHub title="继续观看" count={resume.length} variant="landscape">
              {resume.map((item) => (
                <HomeResumeCard key={item.id} item={item} onPlay={onPlay} />
              ))}
            </HomeHub>
          )}
          {recent.length > 0 && (
            <HomeHub
              title="最近添加"
              count={recent.length}
              variant="poster"
              action={
                <button type="button" className="home-hub__see-all" onClick={onSeeAll}>
                  查看全部
                  <Icon name="chevron-right" size={14} />
                </button>
              }
            >
              {recent.map((item) => (
                <PosterCard key={item.id} item={item} landscape={false} onSelect={onOpen} />
              ))}
            </HomeHub>
          )}
        </>
      )}
    </main>
  );
}
