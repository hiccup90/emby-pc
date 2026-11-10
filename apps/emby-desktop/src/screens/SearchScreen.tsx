import { useEffect, useRef } from "react";
import type { EmbyLibraryItem } from "../emby/useEmby";
import { isPlayableItemType } from "../emby/library";
import { useSearch } from "../emby/useSearch";
import { ErrorNotice, Icon } from "./shared";
import { PosterCard } from "./cards";
import "./SearchScreen.css";

interface SearchScreenProps {
  /** Open the details page for a result. */
  onOpen: (item: EmbyLibraryItem) => void;
  /** Start playback for a playable result (movie/episode/video). */
  onPlay: (item: EmbyLibraryItem) => void;
}

const SKELETON_COUNT = 14;

function SearchIcon({ size = 18 }: { size?: number }) {
  return (
    <svg
      aria-hidden="true"
      width={size}
      height={size}
      viewBox="0 0 24 24"
      fill="none"
      stroke="currentColor"
      strokeWidth={1.8}
      strokeLinecap="round"
      strokeLinejoin="round"
    >
      <circle cx="10.5" cy="10.5" r="6.5" />
      <path d="m15.5 15.5 5 5" />
    </svg>
  );
}

function SearchSkeleton() {
  return (
    <div className="loading-state" role="status" aria-live="polite">
      <span className="visually-hidden">正在搜索…</span>
      <div className="poster-grid" aria-hidden="true">
        {Array.from({ length: SKELETON_COUNT }, (_, index) => (
          <span className="skeleton-card" key={index}>
            <span className="skeleton-block" />
            <span className="skeleton-line" />
            <span className="skeleton-line skeleton-line--short" />
          </span>
        ))}
      </div>
    </div>
  );
}

export default function SearchScreen({ onOpen, onPlay }: SearchScreenProps) {
  const inputRef = useRef<HTMLInputElement>(null);
  const search = useSearch();
  const trimmedQuery = search.query.trim();

  useEffect(() => {
    inputRef.current?.focus();
  }, []);

  const clear = () => {
    search.setQuery("");
    inputRef.current?.focus();
  };

  return (
    <main className="page search-page">
      <div className="search-hero">
        <h1 className="search-hero__title">搜索</h1>
        <div className="search-box">
          <span className="search-box__icon" aria-hidden="true">
            <SearchIcon />
          </span>
          <input
            ref={inputRef}
            autoFocus
            type="search"
            className="search-box__input"
            value={search.query}
            onChange={(event) => search.setQuery(event.target.value)}
            placeholder="搜索电影、剧集或单集"
            aria-label="搜索 Emby 媒体库"
            spellCheck={false}
            autoComplete="off"
          />
          {search.pending && (
            <span className="search-box__spinner spinner spinner--small" role="status" aria-label="正在搜索" />
          )}
          {trimmedQuery && (
            <button
              type="button"
              className="search-box__clear"
              onClick={clear}
              aria-label="清空搜索"
            >
              <svg aria-hidden="true" width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth={2} strokeLinecap="round">
                <path d="m6 6 12 12M18 6 6 18" />
              </svg>
            </button>
          )}
        </div>
      </div>

      {search.status === "idle" ? (
        <div className="search-hint">
          <span className="search-hint__icon" aria-hidden="true">
            <SearchIcon size={30} />
          </span>
          <p className="search-hint__title">输入关键词开始搜索</p>
          <p className="search-hint__text">搜索整个 Emby 媒体库中的电影、剧集与单集。</p>
        </div>
      ) : search.status === "loading" ? (
        <SearchSkeleton />
      ) : search.status === "error" ? (
        <div className="page-message">
          <ErrorNotice
            message={search.error ?? "搜索失败，请稍后重试。"}
            onRetry={search.reload}
            retryLabel="重新搜索"
          />
        </div>
      ) : (
        <section aria-labelledby="search-results-title">
          <div className="page-header">
            <h2 className="page-title" id="search-results-title">
              搜索结果
            </h2>
            <span className="page-count">
              {search.totalItems > search.items.length
                ? `${search.items.length} / ${search.totalItems} 项`
                : `${search.items.length} 项`}
            </span>
          </div>

          {search.items.length === 0 ? (
            <p className="empty-state">没有找到与「{search.debouncedQuery}」相关的内容。</p>
          ) : (
            <div className="poster-grid">
              {search.items.map((item) => (
                <div className="search-result" key={item.id}>
                  <PosterCard item={item} landscape={false} onSelect={onOpen} />
                  {isPlayableItemType(item.type) && (
                    <button
                      type="button"
                      className="search-result__play"
                      onClick={() => onPlay(item)}
                      aria-label={`播放《${item.name}》`}
                      title="播放"
                    >
                      <Icon name="play" size={16} />
                    </button>
                  )}
                </div>
              ))}
            </div>
          )}
        </section>
      )}
    </main>
  );
}
