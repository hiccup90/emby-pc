import { Icon } from "./shared";
import "./LibraryToolbar.css";

/** Emby SortBy values used by the library toolbar. */
export type LibrarySortKey =
  | "SortName"
  | "ProductionYear"
  | "DateCreated"
  | "CommunityRating";

export type LibrarySortOrder = "Ascending" | "Descending";

export type LibraryItemType = "all" | "Movie" | "Series";

/** Everything the library toolbar controls (sort + filters). */
export interface LibraryQuery {
  sortBy: LibrarySortKey;
  sortOrder: LibrarySortOrder;
  /** Hide items the user has already played. */
  unplayedOnly: boolean;
  /** Restrict a mixed library to movies or series; "all" keeps everything. */
  itemType: LibraryItemType;
}

export const DEFAULT_LIBRARY_QUERY: LibraryQuery = {
  sortBy: "SortName",
  sortOrder: "Ascending",
  unplayedOnly: false,
  itemType: "all",
};

export function isDefaultLibraryQuery(query: LibraryQuery): boolean {
  return (
    query.sortBy === DEFAULT_LIBRARY_QUERY.sortBy &&
    query.sortOrder === DEFAULT_LIBRARY_QUERY.sortOrder &&
    query.unplayedOnly === DEFAULT_LIBRARY_QUERY.unplayedOnly &&
    query.itemType === DEFAULT_LIBRARY_QUERY.itemType
  );
}

const SORT_OPTIONS: ReadonlyArray<{ value: LibrarySortKey; label: string }> = [
  { value: "SortName", label: "名称" },
  { value: "ProductionYear", label: "年份" },
  { value: "DateCreated", label: "添加时间" },
  { value: "CommunityRating", label: "评分" },
];

const TYPE_OPTIONS: ReadonlyArray<{ value: LibraryItemType; label: string }> = [
  { value: "all", label: "全部" },
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
  value: LibraryQuery;
  onChange: (next: LibraryQuery) => void;
  /**
   * Item types the current view can contain. The type filter is hidden when
   * the library is already limited to a single type.
   */
  availableTypes: ReadonlyArray<"Movie" | "Series">;
}

/**
 * Plex-style library toolbar: sort key + direction, an "unplayed only" switch
 * and (for mixed libraries) a movie/series type filter. Presentational only;
 * the parent owns the query state and forwards it to the data layer.
 */
export default function LibraryToolbar({ value, onChange, availableTypes }: LibraryToolbarProps) {
  const showTypeFilter = availableTypes.length > 1;
  const descending = value.sortOrder === "Descending";
  const patch = (next: Partial<LibraryQuery>) => onChange({ ...value, ...next });

  return (
    <div className="library-toolbar" role="toolbar" aria-label="媒体库筛选与排序">
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

      <div className="library-toolbar__group">
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
      </div>

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
            {TYPE_OPTIONS.filter(
              (option) => option.value === "all" || availableTypes.includes(option.value),
            ).map((option) => (
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
