import { useCallback, useEffect, useState } from "react";
import { describeEmbyError, embyImageUrl, embyItems } from "./client";
import type { EmbyItem } from "./contract";
import type { EmbyLibraryItem } from "./useEmby";

/** Debounce used by the search box before a request is sent. */
export const SEARCH_DEBOUNCE_MS = 250;
/** The search screen shows one flat result page. */
export const SEARCH_LIMIT = 60;
export const SEARCH_ITEM_TYPES = "Movie,Series,Episode";

export type SearchStatus = "idle" | "loading" | "ready" | "error";

async function safeImageUrl(
  item: EmbyItem,
  imageType: "Primary" | "Backdrop",
  maxWidth: number,
): Promise<string | null> {
  const tag =
    imageType === "Primary" ? item.imageTags.Primary : item.backdropImageTags[0];
  if (!tag) return null;

  try {
    const url = await embyImageUrl({ itemId: item.id, imageType, maxWidth, tag });
    return url.trim() ? url : null;
  } catch (reason) {
    console.warn(`[emby] could not build ${imageType} image URL`, reason);
    return null;
  }
}

/**
 * Convert raw Emby items into the shape `PosterCard` expects, resolving the
 * poster (and optionally backdrop) URLs through the Tauri image bridge.
 */
export async function decorateItems(
  items: EmbyItem[],
  withBackdrop = false,
): Promise<EmbyLibraryItem[]> {
  return Promise.all(
    items.map(async (item) => ({
      ...item,
      posterUrl: await safeImageUrl(item, "Primary", 400),
      backdropUrl: withBackdrop ? await safeImageUrl(item, "Backdrop", 1280) : null,
    })),
  );
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
 */
export function useSearch(debounceMs: number = SEARCH_DEBOUNCE_MS): EmbySearchController {
  const [query, setQuery] = useState("");
  const [debouncedQuery, setDebouncedQuery] = useState("");
  const [items, setItems] = useState<EmbyLibraryItem[]>([]);
  const [totalItems, setTotalItems] = useState(0);
  const [status, setStatus] = useState<SearchStatus>("idle");
  const [error, setError] = useState<string | null>(null);
  const [reloadToken, setReloadToken] = useState(0);

  useEffect(() => {
    const timer = window.setTimeout(() => setDebouncedQuery(query.trim()), debounceMs);
    return () => window.clearTimeout(timer);
  }, [query, debounceMs]);

  useEffect(() => {
    if (!debouncedQuery) {
      setItems([]);
      setTotalItems(0);
      setStatus("idle");
      setError(null);
      return undefined;
    }

    let cancelled = false;
    setItems([]);
    setTotalItems(0);
    setStatus("loading");
    setError(null);

    void (async () => {
      try {
        const page = await embyItems({
          searchTerm: debouncedQuery,
          recursive: true,
          includeItemTypes: SEARCH_ITEM_TYPES,
          limit: SEARCH_LIMIT,
        });
        const decorated = await decorateItems(page.items);
        if (cancelled) return;
        setItems(decorated);
        setTotalItems(page.totalRecordCount);
        setStatus("ready");
      } catch (reason) {
        if (cancelled) return;
        setItems([]);
        setTotalItems(0);
        setError(describeEmbyError(reason, "搜索失败，请稍后重试。"));
        setStatus("error");
      }
    })();

    return () => {
      cancelled = true;
    };
  }, [debouncedQuery, reloadToken]);

  const reload = useCallback(() => setReloadToken((value) => value + 1), []);

  return {
    query,
    setQuery,
    debouncedQuery,
    items,
    totalItems,
    status,
    error,
    pending: status === "loading" || query.trim() !== debouncedQuery,
    reload,
  };
}
