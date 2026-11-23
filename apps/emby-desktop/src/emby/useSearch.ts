import { useEffect, useRef, useState } from "react";
import { embyItems } from "./client";
import type { EmbyLibraryItem } from "./useEmby";
import { decorateItems } from "./artwork";
import { useAsyncResource } from "./useAsyncResource";

/** Debounce used by the search box before a request is sent. */
const SEARCH_DEBOUNCE_MS = 250;
/** The search screen shows one flat result page. */
const SEARCH_LIMIT = 60;
const SEARCH_ITEM_TYPES = "Movie,Series,Episode";

export type SearchStatus = "idle" | "loading" | "ready" | "error";

interface SearchResults {
  items: EmbyLibraryItem[];
  totalItems: number;
}

export interface UseSearchOptions {
  /** Debounce before a request is sent. Defaults to 250ms. */
  debounceMs?: number;
  /** Starting text; only read on mount. */
  initialQuery?: string;
  /** Called with every raw query change, so a parent can remember it. */
  onQueryChange?: (query: string) => void;
  /** Bumped by the shell after a delete/identify to re-read the same query. */
  refreshKey?: number;
}

export interface EmbySearchController {
  /** Raw text currently in the search field. */
  query: string;
  setQuery: (query: string) => void;
  /** Trimmed text of the most recent request. */
  debouncedQuery: string;
  items: EmbyLibraryItem[];
  totalItems: number;
  status: SearchStatus;
  error: string | null;
  /** The field has pending input or a request is in flight. */
  pending: boolean;
  reload: () => void;
}

/**
 * Debounced full-library search over movies, series and episodes.
 * Empty queries stay idle; each non-empty query replaces the previous results.
 * The query is uncontrolled by default; `initialQuery` + `onQueryChange` let a
 * parent restore and remember it without taking over the field state.
 */
export function useSearch(options: UseSearchOptions = {}): EmbySearchController {
  const {
    debounceMs = SEARCH_DEBOUNCE_MS,
    initialQuery = "",
    onQueryChange,
    refreshKey = 0,
  } = options;
  const [query, setQueryState] = useState(initialQuery);
  const [debouncedQuery, setDebouncedQuery] = useState(initialQuery.trim());
  const onQueryChangeRef = useRef(onQueryChange);
  onQueryChangeRef.current = onQueryChange;

  const setQuery = (next: string) => {
    setQueryState(next);
    onQueryChangeRef.current?.(next);
  };

  useEffect(() => {
    const timer = window.setTimeout(() => setDebouncedQuery(query.trim()), debounceMs);
    return () => window.clearTimeout(timer);
  }, [query, debounceMs]);

  const resource = useAsyncResource<SearchResults>(
    async (isCancelled) => {
      const page = await embyItems({
        searchTerm: debouncedQuery,
        recursive: true,
        includeItemTypes: SEARCH_ITEM_TYPES,
        limit: SEARCH_LIMIT,
      });
      if (isCancelled()) return { items: [], totalItems: 0 };
      const items = await decorateItems(page.items);
      return { items, totalItems: page.totalRecordCount };
    },
    `search:${debouncedQuery}:${refreshKey}`,
    { enabled: Boolean(debouncedQuery), fallbackError: "搜索失败，请稍后重试。" },
  );

  return {
    query,
    setQuery,
    debouncedQuery,
    items: resource.data?.items ?? [],
    totalItems: resource.data?.totalItems ?? 0,
    status: resource.status,
    error: resource.error,
    pending: resource.status === "loading" || query.trim() !== debouncedQuery,
    reload: resource.reload,
  };
}