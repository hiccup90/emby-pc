import type {
  EmbyLibraryItemType,
  EmbyLibraryQuery,
  EmbyLibrarySortBy,
} from "../emby/useEmby";
import { DEFAULT_LIBRARY_QUERY, isDefaultLibraryQuery } from "../emby/useEmby";
import { Icon } from "./shared";
import "./LibraryToolbar.css";

const SORT_OPTIONS: ReadonlyArray<{ value: EmbyLibrarySortBy; label: string }> = [
  { value: "SortName", label: "名称" },
  { value: "ProductionYear", label: "年份" },
  { value: "DateCreated", label: "添加时间" },
  { value: "CommunityRating", label: "评分" },
];

const TYPE_OPTIONS: ReadonlyArray<{ value: EmbyLibraryItemType; label: string }> = [
  { value: "Movie", label: "电影" },
  { value: "Series", label: "剧集" },
];

function SortArrow({ ascending }: { ascending: boolean }) {
  return (
    <svg
      aria-hidden="true"
      width={14}
      height={14}
      viewBox="0 0 24 24"
      fill="none"
      stroke="currentColor"
      strokeWidth={2}
      strokeLinecap="round"
      strokeLinejoin="round"
    >
      {ascending ? <path d="M12 19V5m0 0-5 5m5-5 5 5" /> : <path d="M12 5v14m0 0-5-5m5 5 5-5" />}
    </svg>
  );
}

export interface LibraryToolbarProps {
  value: EmbyLibraryQuery;
  onChange: (next: EmbyLibraryQuery) => void;
  /**
   * Only mixed libraries (movies + series in one view) can be narrowed by
   * item type; for every other view the filter stays hidden.
   */
  showTypeFilter: boolean;
  /** Query results are loading; the toolbar stays usable but is marked busy. */
  busy?: boolean;
}

/**
 * Plex-style library toolbar: sort key + direction, an "unplayed only" switch
 * and, for mixed libraries, a movie/series type filter. Presentational only;
 * every change goes straight to the library data layer.
 */
export default function LibraryToolbar({
  value,
  onChange,
  showTypeFilter,
  busy = false,
}: LibraryToolbarProps) {
  const descending = value.sortOrder === "Descending";
  const patch = (next: Partial<EmbyLibraryQuery>) => onChange({ ...value, ...next });

  return (
    <div
      className={`library-toolbar${busy ? " is-busy" : ""}`}
      role="toolbar"
      aria-label="媒体库筛选与排序"
      aria-busy={busy}
    >
      <div className="library-toolbar__group">
        <span className="library-toolbar__label">排序</span>
        <div className="library-toolbar__segments" role="group" aria-label="排序方式">
          {SORT_OPTIONS.map((option) => (
            <button
              key={option.value}
              type="button"
              className={`library-toolbar__segment${value.sortBy === option.value ? " is-active" : ""}`}
              aria-pressed={value.sortBy === option.value}
              onClick={() => patch({ sortBy: option.value })}
            >
              {option.label}
            </button>
          ))}
        </div>
      </div>

      <button
        type="button"
        className="library-toolbar__direction"
        aria-label={`排序方向：${descending ? "降序" : "升序"}，点击切换`}
        title="切换排序方向"
        onClick={() => patch({ sortOrder: descending ? "Ascending" : "Descending" })}
      >
        <SortArrow ascending={!descending} />
        <span>{descending ? "降序" : "升序"}</span>
      </button>

      <button
        type="button"
        className="library-toolbar__switch"
        role="switch"
        aria-checked={value.unplayedOnly}
        onClick={() => patch({ unplayedOnly: !value.unplayedOnly })}
      >
        <span className="library-toolbar__switch-track" aria-hidden="true">
          <span className="library-toolbar__switch-thumb" />
        </span>
        <span>仅未看</span>
      </button>

      {showTypeFilter && (
        <div className="library-toolbar__group">
          <span className="library-toolbar__label">类型</span>
          <div className="library-toolbar__segments" role="group" aria-label="类型筛选">
            <button
              type="button"
              className={`library-toolbar__segment${value.itemType === "all" ? " is-active" : ""}`}
              aria-pressed={value.itemType === "all"}
              onClick={() => patch({ itemType: "all" })}
            >
              全部
            </button>
            {TYPE_OPTIONS.map((option) => (
              <button
                key={option.value}
                type="button"
                className={`library-toolbar__segment${value.itemType === option.value ? " is-active" : ""}`}
                aria-pressed={value.itemType === option.value}
                onClick={() => patch({ itemType: option.value })}
              >
                {option.label}
              </button>
            ))}
          </div>
        </div>
      )}

      {!isDefaultLibraryQuery(value) && (
        <button
          type="button"
          className="library-toolbar__reset"
          onClick={() => onChange(DEFAULT_LIBRARY_QUERY)}
        >
          <Icon name="refresh" size={14} />
          重置
        </button>
      )}
    </div>
  );
}
