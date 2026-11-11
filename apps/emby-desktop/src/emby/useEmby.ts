import { useCallback, useEffect, useRef, useState } from "react";
import {
  describeEmbyError,
  embyImageUrl,
  embyItem,
  embyItems,
  embyLogin,
  embyLogout,
  embyPlaybackTarget,
  embyResume,
  embySessionBoot,
  embySimilar,
  embyViews,
  hasTauriRuntime,
  onEmbySessionEvent,
} from "./client";
import type { EmbyCredentials } from "./client";
import type {
  EmbyItem,
  EmbyPerson,
  EmbyPlaybackTarget,
  EmbySession,
  EmbySessionBoot,
  EmbySessionReason,
  EmbySortOrder,
  EmbyView,
} from "./contract";
import {
  LIBRARY_MAX_ITEMS,
  LIBRARY_PAGE_SIZE,
  RESUME_LIMIT,
  includeItemTypesForView,
  pickDefaultView,
} from "./library";

export interface EmbyLibraryItem extends EmbyItem {
  posterUrl: string | null;
  backdropUrl: string | null;
}

export type EmbyLoadStatus = "idle" | "loading" | "ready" | "error";

export interface EmbyLibraryState {
  status: EmbyLoadStatus;
  view: EmbyView | null;
  views: EmbyView[];
  items: EmbyLibraryItem[];
  resumable: EmbyLibraryItem[];
  totalItems: number;
  loadingMore: boolean;
  hasMore: boolean;
  error: string | null;
}

export interface EmbyLibraryController extends EmbyLibraryState {
  /** True only for views the server marks as mixed content (movies + series). */
  supportsTypeFilter: boolean;
  /** Continue-watching load state; the row is best-effort and may stay empty. */
  resumeStatus: EmbyLoadStatus;
  /** Why the continue-watching row is empty, when it is a failure, not a fact. */
  resumeError: string | null;
  selectView: (viewId: string) => void;
  loadMore: () => void;
  reload: () => void;
  /** Re-read the continue-watching row after its request failed. */
  retryResume: () => void;
  refreshResume: () => void;
}

/** Emby `SortBy` values offered by the library toolbar. */
export type EmbyLibrarySortBy =
  | "SortName"
  | "ProductionYear"
  | "DateCreated"
  | "CommunityRating";

/** Item type restriction; `"all"` keeps whatever the view contains. */
export type EmbyLibraryItemType = "all" | "Movie" | "Series";

/**
 * Everything the toolbar controls. The hook sends it to the server and keys
 * its page cache by it, so two different queries never share loaded items.
 */
export interface EmbyLibraryQuery {
  sortBy: EmbyLibrarySortBy;
  sortOrder: EmbySortOrder;
  /** Server-side `Filters=IsUnplayed`. */
  unplayedOnly: boolean;
  /** Only applied to genuinely mixed views (see `supportsTypeFilter`). */
  itemType: EmbyLibraryItemType;
}

export const DEFAULT_LIBRARY_QUERY: EmbyLibraryQuery = {
  sortBy: "SortName",
  sortOrder: "Ascending",
  unplayedOnly: false,
  itemType: "all",
};

export function isDefaultLibraryQuery(query: EmbyLibraryQuery): boolean {
  return (
    query.sortBy === DEFAULT_LIBRARY_QUERY.sortBy &&
    query.sortOrder === DEFAULT_LIBRARY_QUERY.sortOrder &&
    query.unplayedOnly === DEFAULT_LIBRARY_QUERY.unplayedOnly &&
    query.itemType === DEFAULT_LIBRARY_QUERY.itemType
  );
}

export interface EmbyLibraryOptions {
  query?: EmbyLibraryQuery;
  /** Items per page; comes from `AppSettings.pageSize`. */
  pageSize?: number;
}

/**
 * Emby's "mixed content" library. Emby (and Jellyfin) represent it as a view
 * *without* a collection type — the library picker's "Mixed content" entry —
 * while a few servers echo "mixed". Those are the only views whose item types
 * are unknown, which is exactly why the Movie/Series filter exists.
 *
 * An unrecognised collection type (books, photos, musicvideos, boxsets, plugin
 * libraries) is NOT mixed: those views have a known content type of their own.
 */
function isMixedCollection(view: Pick<EmbyView, "collectionType"> | null | undefined): boolean {
  const value = view?.collectionType?.trim().toLowerCase();
  return !value || value === "mixed";
}

/** Only the mixed-content library can be narrowed by item type. */
function viewSupportsTypeFilter(view: EmbyView | null): boolean {
  return isMixedCollection(view);
}

/** `IncludeItemTypes` for one page request, honouring the mixed-view filter. */
function includeItemTypesForQuery(
  view: EmbyView,
  itemType: EmbyLibraryItemType,
): string | undefined {
  if (itemType !== "all" && viewSupportsTypeFilter(view)) return itemType;
  // "All" always means "whatever the view holds": a mixed library may contain
  // more than movies and series, and fixed-type views ignore the filter.
  return includeItemTypesForView(view);
}

function normalizePageSize(value: number | undefined): number {
  if (typeof value !== "number" || !Number.isFinite(value)) return LIBRARY_PAGE_SIZE;
  const size = Math.floor(value);
  return size > 0 ? size : LIBRARY_PAGE_SIZE;
}

/** Stable string for one query + page size; used as the cache key prefix. */
function libraryQuerySignature(query: EmbyLibraryQuery, pageSize: number): string {
  return [
    query.sortBy,
    query.sortOrder,
    query.unplayedOnly ? "unplayed" : "all",
    query.itemType,
    pageSize,
  ].join("|");
}

function cacheKeyFor(viewId: string, signature: string): string {
  return `${viewId}::${signature}`;
}

/** Single boot/session state machine shared by the shell and the login page. */
export type EmbySessionState =
  | { status: "booting" }
  | { status: "signedIn"; session: EmbySession }
  | {
      status: "signedOut";
      reason: EmbySessionReason;
      lastServer: string | null;
      lastUserName: string | null;
    };

export interface EmbySessionController {
  state: EmbySessionState;
  /** Convenience for screens that only render while signed in. */
  session: EmbySession | null;
  loginPending: boolean;
  logoutPending: boolean;
  loginError: string | null;
  signIn: (credentials: EmbyCredentials) => Promise<boolean>;
  signOut: () => Promise<void>;
  /** Retry the startup boot with force=true (bypasses the backend's cache). */
  retryBoot: () => Promise<void>;
  clearLoginError: () => void;
}

export interface EmbyDetailState {
  item: EmbyLibraryItem | null;
  /** Cast/crew photo URLs by person id; filled after the item has loaded. */
  photos: Record<string, string>;
  loading: boolean;
  error: string | null;
  reload: () => void;
}

export interface EmbyEpisodesState {
  items: EmbyLibraryItem[];
  loading: boolean;
  error: string | null;
  reload: () => void;
}

export interface PlaybackLauncher {
  pending: boolean;
  error: string | null;
  launch: (itemId: string) => Promise<EmbyPlaybackTarget | null>;
  clearError: () => void;
}

interface ViewCache {
  items: EmbyLibraryItem[];
  total: number;
  status: EmbyLoadStatus;
  error: string | null;
  loadingMore: boolean;
}

interface LibraryStore {
  views: EmbyView[];
  viewsStatus: EmbyLoadStatus;
  viewsError: string | null;
  viewId: string | null;
  /** Cache is keyed by view id + query signature (see `cacheKeyFor`). */
  cache: Record<string, ViewCache>;
  /** Signature of the query the cached pages belong to. */
  queryKey: string;
  resumable: EmbyLibraryItem[];
  resumeStatus: EmbyLoadStatus;
  resumeError: string | null;
}

function emptyStore(): LibraryStore {
  return {
    views: [],
    viewsStatus: "idle",
    viewsError: null,
    viewId: null,
    cache: {},
    queryKey: "",
    resumable: [],
    resumeStatus: "idle",
    resumeError: null,
  };
}

async function safeImageUrl(
  item: EmbyItem,
  imageType: "Primary" | "Backdrop",
  maxWidth: number,
): Promise<string | null> {
  const tag =
    imageType === "Primary" ? item.imageTags.Primary : item.backdropImageTags[0];
  if (!tag) return null;
  const request = {
    itemId: item.id,
    imageType,
    maxWidth,
    ...(tag ? { tag } : {}),
  };

  try {
    const url = await embyImageUrl(request);
    return url.trim() ? url : null;
  } catch (reason) {
    console.warn(`[emby] could not build ${imageType} image URL`, reason);
    return null;
  }
}

async function decorateItem(
  item: EmbyItem,
  includeBackdrop: boolean,
  posterWidth = 400,
): Promise<EmbyLibraryItem> {
  const [posterUrl, backdropUrl] = await Promise.all([
    safeImageUrl(item, "Primary", posterWidth),
    includeBackdrop ? safeImageUrl(item, "Backdrop", 1280) : Promise.resolve(null),
  ]);
  return { ...item, posterUrl, backdropUrl };
}

/** One image URL per cast/crew member that has a primary image. */
async function personPhotoUrls(people: EmbyPerson[]): Promise<Record<string, string>> {
  const withPhoto = people.filter((person) => person.id && person.primaryImageTag).slice(0, 40);
  const entries = await Promise.all(
    withPhoto.map(async (person): Promise<[string, string] | null> => {
      const id = person.id as string;
      try {
        const url = await embyImageUrl({
          itemId: id,
          imageType: "Primary",
          maxWidth: 240,
          tag: person.primaryImageTag as string,
        });
        return url.trim() ? [id, url] : null;
      } catch (reason) {
        console.warn("[emby] could not build a person image URL", reason);
        return null;
      }
    }),
  );
  return Object.fromEntries(entries.filter((entry): entry is [string, string] => entry !== null));
}

function plainLibraryItem(item: EmbyItem): EmbyLibraryItem {
  return { ...item, posterUrl: null, backdropUrl: null };
}

/** Map a structured command error to a session reason; unknown errors are server errors. */
function sessionReasonFromError(error: unknown): EmbySessionReason {
  if (typeof error === "object" && error !== null && "reason" in error) {
    const reason = (error as { reason?: unknown }).reason;
    if (
      reason === "none" ||
      reason === "token_rejected" ||
      reason === "unreachable" ||
      reason === "server_error" ||
      reason === "storage_error" ||
      reason === "invalid_credentials" ||
      reason === "invalid_request"
    ) {
      return reason;
    }
  }
  return "server_error";
}

export function useEmbySession(): EmbySessionController {
  const desktop = hasTauriRuntime();
  const [state, setState] = useState<EmbySessionState>(() =>
    desktop
      ? { status: "booting" }
      : { status: "signedOut", reason: "none", lastServer: null, lastUserName: null },
  );
  const [loginPending, setLoginPending] = useState(false);
  const [logoutPending, setLogoutPending] = useState(false);
  const [loginError, setLoginError] = useState<string | null>(null);
  const stateRef = useRef(state);
  stateRef.current = state;

  const applyBootResult = useCallback((result: EmbySessionBoot) => {
    if (result.status === "signedIn" && result.session) {
      setState({ status: "signedIn", session: result.session });
      return;
    }
    setState({
      status: "signedOut",
      reason: result.reason,
      lastServer: result.lastServer ?? null,
      lastUserName: result.lastUserName ?? null,
    });
  }, []);

  // One startup boot: the backend owns retry/backoff and single-flight; the UI
  // shows BootScreen until it resolves.
  useEffect(() => {
    if (!desktop) {
      setState({
        status: "signedOut",
        reason: "none",
        lastServer: null,
        lastUserName: null,
      });
      return undefined;
    }

    let cancelled = false;
    setState({ status: "booting" });
    void embySessionBoot()
      .then((result) => {
        if (!cancelled) applyBootResult(result);
      })
      .catch((reason: unknown) => {
        if (cancelled) return;
        setState({
          status: "signedOut",
          reason: sessionReasonFromError(reason),
          lastServer: null,
          lastUserName: null,
        });
      });
    return () => {
      cancelled = true;
    };
  }, [applyBootResult, desktop]);

  // The backend emits this only after a running command confirmed the token is
  // rejected. Keep prefill hints from the in-memory session before dropping it.
  useEffect(() => {
    if (!desktop) return undefined;
    let disposed = false;
    let stop: (() => void) | null = null;
    void onEmbySessionEvent((event) => {
      if (disposed) return;
      const current = stateRef.current;
      const session = current.status === "signedIn" ? current.session : null;
      setState({
        status: "signedOut",
        reason: event.reason,
        lastServer:
          session?.serverUrl ??
          (current.status === "signedOut" ? current.lastServer : null),
        lastUserName:
          session?.userName ??
          (current.status === "signedOut" ? current.lastUserName : null),
      });
      setLoginError(null);
      setLoginPending(false);
      setLogoutPending(false);
    })
      .then((unlisten) => {
        if (disposed) unlisten();
        else stop = unlisten;
      })
      .catch((reason: unknown) => {
        console.warn("[emby] could not subscribe to session events", reason);
      });
    return () => {
      disposed = true;
      stop?.();
    };
  }, [desktop]);

  const retryBoot = useCallback(async (): Promise<void> => {
    if (!desktop) return;
    const previous = stateRef.current;
    const lastServer = previous.status === "signedOut" ? previous.lastServer : null;
    const lastUserName = previous.status === "signedOut" ? previous.lastUserName : null;
    setState({ status: "booting" });
    setLoginError(null);
    try {
      applyBootResult(await embySessionBoot(true));
    } catch (reason) {
      setState({
        status: "signedOut",
        reason: sessionReasonFromError(reason),
        lastServer,
        lastUserName,
      });
    }
  }, [applyBootResult, desktop]);

  const signIn = useCallback(
    async (credentials: EmbyCredentials): Promise<boolean> => {
      setLoginError(null);
      if (!hasTauriRuntime()) {
        setLoginError("预览模式：需要 Tauri 桌面环境连接 Emby。");
        return false;
      }

      setLoginPending(true);
      try {
        const next = await embyLogin(credentials);
        setState({ status: "signedIn", session: next });
        return true;
      } catch (reason) {
        setLoginError(
          describeEmbyError(reason, "登录失败，请检查服务器地址与凭据。"),
        );
        return false;
      } finally {
        setLoginPending(false);
      }
    },
    [],
  );

  const signOut = useCallback(async (): Promise<void> => {
    setLogoutPending(true);
    const previous = stateRef.current;
    const session = previous.status === "signedIn" ? previous.session : null;
    try {
      if (hasTauriRuntime()) await embyLogout();
    } catch (reason) {
      console.warn("[emby] logout failed; clearing the local session anyway", reason);
    } finally {
      setState({
        status: "signedOut",
        reason: "none",
        lastServer: session?.serverUrl ?? null,
        lastUserName: session?.userName ?? null,
      });
      setLoginPending(false);
      setLoginError(null);
      setLogoutPending(false);
    }
  }, []);

  const clearLoginError = useCallback(() => setLoginError(null), []);

  return {
    state,
    session: state.status === "signedIn" ? state.session : null,
    loginPending,
    logoutPending,
    loginError,
    signIn,
    signOut,
    retryBoot,
    clearLoginError,
  };
}

export function useEmbyLibrary(
  session: EmbySession | null,
  enabled: boolean,
  options: EmbyLibraryOptions = {},
): EmbyLibraryController {
  const desktop = hasTauriRuntime();
  const [store, setStore] = useState<LibraryStore>(emptyStore);
  const storeRef = useRef<LibraryStore>(store);
  const generationRef = useRef(0);
  const requestsRef = useRef<Set<string>>(new Set());
  const resumeRefreshRef = useRef(false);
  const [reloadToken, setReloadToken] = useState(0);

  const query = options.query ?? DEFAULT_LIBRARY_QUERY;
  const pageSize = normalizePageSize(options.pageSize);
  const querySignature = libraryQuerySignature(query, pageSize);
  const queryRef = useRef({ query, pageSize });
  queryRef.current = { query, pageSize };
  const querySignatureRef = useRef(querySignature);
  querySignatureRef.current = querySignature;

  const updateStore = useCallback(
    (updater: (current: LibraryStore) => LibraryStore) => {
      setStore((current) => {
        const next = updater(current);
        storeRef.current = next;
        return next;
      });
    },
    [],
  );

  const fetchViewPage = useCallback(
    async (view: EmbyView, mode: "replace" | "append") => {
      const { query: activeQuery, pageSize: activePageSize } = queryRef.current;
      const key = cacheKeyFor(view.id, libraryQuerySignature(activeQuery, activePageSize));
      const generation = generationRef.current;
      const requestKey = `${generation}:${key}`;
      if (requestsRef.current.has(requestKey)) return;

      const existing = storeRef.current.cache[key];
      const startIndex = mode === "append" ? existing?.items.length ?? 0 : 0;
      if (mode === "append") {
        if (!existing || existing.status !== "ready" || existing.loadingMore) return;
        if (startIndex >= Math.min(existing.total, LIBRARY_MAX_ITEMS)) return;
      }

      requestsRef.current.add(requestKey);
      updateStore((current) => {
        const previous = current.cache[key];
        return {
          ...current,
          cache: {
            ...current.cache,
            [key]: {
              items: mode === "replace" ? [] : previous?.items ?? [],
              total: previous?.total ?? 0,
              status: mode === "replace" ? "loading" : "ready",
              error: null,
              loadingMore: mode === "append",
            },
          },
        };
      });

      try {
        const includeItemTypes = includeItemTypesForQuery(view, activeQuery.itemType);
        const page = await embyItems({
          parentId: view.id,
          recursive: true,
          sortBy: activeQuery.sortBy,
          sortOrder: activeQuery.sortOrder,
          limit: activePageSize,
          startIndex,
          ...(includeItemTypes ? { includeItemTypes } : {}),
          ...(activeQuery.unplayedOnly ? { filters: "IsUnplayed" } : {}),
        });
        const decorated = await Promise.all(
          page.items.map((item) => decorateItem(item, false)),
        );
        if (generation !== generationRef.current) return;

        updateStore((current) => {
          const previous = current.cache[key];
          const base = mode === "append" ? previous?.items ?? [] : [];
          const seen = new Set(base.map((item) => item.id));
          const merged = [...base];
          for (const item of decorated) {
            if (seen.has(item.id)) continue;
            seen.add(item.id);
            merged.push(item);
          }
          const stalled = mode === "append" && merged.length === base.length;
          return {
            ...current,
            cache: {
              ...current.cache,
              [key]: {
                items: merged,
                total: stalled ? merged.length : page.totalRecordCount,
                status: "ready",
                error: null,
                loadingMore: false,
              },
            },
          };
        });
      } catch (reason) {
        if (generation !== generationRef.current) return;
        const message = describeEmbyError(reason, "无法加载媒体库内容。");
        updateStore((current) => {
          const previous = current.cache[key];
          if (mode === "append") {
            return {
              ...current,
              cache: {
                ...current.cache,
                [key]: {
                  items: previous?.items ?? [],
                  total: previous?.total ?? 0,
                  status: "ready",
                  error: message,
                  loadingMore: false,
                },
              },
            };
          }
          return {
            ...current,
            cache: {
              ...current.cache,
              [key]: {
                items: [],
                total: 0,
                status: "error",
                error: message,
                loadingMore: false,
              },
            },
          };
        });
      } finally {
        requestsRef.current.delete(requestKey);
      }
    },
    [updateStore],
  );

  useEffect(() => {
    if (!enabled || !session || !desktop) {
      generationRef.current += 1;
      requestsRef.current.clear();
      const empty = emptyStore();
      storeRef.current = empty;
      setStore(empty);
      return;
    }

    const generation = ++generationRef.current;
    requestsRef.current.clear();
    const loading = {
      ...emptyStore(),
      viewsStatus: "loading" as const,
      resumeStatus: "loading" as const,
      queryKey: querySignatureRef.current,
    };
    storeRef.current = loading;
    setStore(loading);

    const load = async () => {
      try {
        const [viewsResult, resumeResult] = await Promise.allSettled([
          embyViews(),
          embyResume({ limit: RESUME_LIMIT }),
        ]);
        if (generation !== generationRef.current) return;
        if (viewsResult.status === "rejected") throw viewsResult.reason;

        let resumable: EmbyLibraryItem[] = [];
        let resumeFailure: string | null = null;
        if (resumeResult.status === "fulfilled") {
          resumable = await Promise.all(
            resumeResult.value.items.map((item) => decorateItem(item, true, 640)),
          );
          if (generation !== generationRef.current) return;
        } else {
          console.warn("[emby] continue-watching request failed", resumeResult.reason);
          resumeFailure = describeEmbyError(
            resumeResult.reason,
            "无法读取继续观看列表。",
          );
        }

        const views = viewsResult.value;
        const view = pickDefaultView(views);
        updateStore((current) => ({
          ...current,
          views,
          viewsStatus: "ready",
          viewsError: null,
          viewId: view?.id ?? null,
          resumable,
          resumeStatus: resumeFailure ? "error" : "ready",
          resumeError: resumeFailure,
        }));

        if (view) await fetchViewPage(view, "replace");
      } catch (reason) {
        if (generation !== generationRef.current) return;
        updateStore((current) => ({
          ...current,
          viewsStatus: "error",
          viewsError: describeEmbyError(reason, "无法读取 Emby 媒体库列表。"),
          resumeStatus: "ready",
        }));
      }
    };

    void load();
    return () => {
      if (generationRef.current === generation) generationRef.current += 1;
    };
  }, [desktop, enabled, fetchViewPage, reloadToken, session, updateStore]);

  // A sort/filter/pageSize change is a new server query: drop the pages that
  // were loaded under the old signature and fetch page 0 again. Views are
  // untouched, so the sidebar and the selected tab survive.
  useEffect(() => {
    const current = storeRef.current;
    if (current.queryKey === querySignature) return;
    const view = current.views.find((candidate) => candidate.id === current.viewId) ?? null;
    if (current.viewsStatus !== "ready" || !view) {
      // Boot/reload is still running (settings arrive asynchronously, so a new
      // page size can land mid-boot). Do not touch the in-flight generation —
      // just record the signature: the running boot reads the newest query from
      // the ref when it eventually fetches page 0.
      updateStore((state) => ({ ...state, queryKey: querySignature }));
      return;
    }
    requestsRef.current.clear();
    generationRef.current += 1;
    updateStore((state) => ({
      ...state,
      cache: {
        [cacheKeyFor(view.id, querySignature)]: {
          items: [],
          total: 0,
          status: "loading",
          error: null,
          loadingMore: false,
        },
      },
      queryKey: querySignature,
    }));
    void fetchViewPage(view, "replace");
  }, [fetchViewPage, querySignature, updateStore]);

  const selectView = useCallback(
    (viewId: string) => {
      const current = storeRef.current;
      const view = current.views.find((candidate) => candidate.id === viewId);
      if (!view) return;
      if (current.viewId !== viewId) {
        updateStore((state) => ({ ...state, viewId }));
      }
      const key = cacheKeyFor(viewId, querySignatureRef.current);
      const cache = current.cache[key];
      if (!cache || cache.status === "error") {
        void fetchViewPage(view, "replace");
      }
    },
    [fetchViewPage, updateStore],
  );

  const loadMore = useCallback(() => {
    const current = storeRef.current;
    const view = current.views.find((candidate) => candidate.id === current.viewId);
    const cache = current.viewId
      ? current.cache[cacheKeyFor(current.viewId, querySignatureRef.current)]
      : undefined;
    if (!view || !cache || cache.status !== "ready" || cache.loadingMore) return;
    if (cache.error) return;
    if (cache.items.length >= cache.total) return;
    if (cache.items.length >= LIBRARY_MAX_ITEMS) return;
    void fetchViewPage(view, "append");
  }, [fetchViewPage]);

  const reload = useCallback(() => {
    const current = storeRef.current;
    if (current.viewsStatus === "error" || current.views.length === 0) {
      setReloadToken((value) => value + 1);
      return;
    }
    const currentView = current.views.find(
      (candidate) => candidate.id === current.viewId,
    );
    if (currentView) void fetchViewPage(currentView, "replace");
  }, [fetchViewPage]);

  // A failed continue-watching request is retried through the boot effect, so
  // the row, the views and the current page are re-read together.
  const retryResume = useCallback(() => setReloadToken((value) => value + 1), []);

  const refreshResume = useCallback(() => {
    if (!enabled || !session || !desktop || resumeRefreshRef.current) return;
    resumeRefreshRef.current = true;
    const generation = generationRef.current;

    void (async () => {
      try {
        const page = await embyResume({ limit: RESUME_LIMIT });
        if (generation !== generationRef.current) return;
        const resumable = await Promise.all(
          page.items.map((item) => decorateItem(item, true, 640)),
        );
        if (generation !== generationRef.current) return;
        updateStore((current) => ({ ...current, resumable }));
      } catch (reason) {
        console.warn(
          "[emby] continue-watching refresh failed; keeping cached items",
          reason,
        );
      } finally {
        resumeRefreshRef.current = false;
      }
    })();
  }, [desktop, enabled, session, updateStore]);

  const view = store.views.find((candidate) => candidate.id === store.viewId) ?? null;
  const cache = store.viewId
    ? store.cache[cacheKeyFor(store.viewId, querySignature)]
    : undefined;
  const status: EmbyLoadStatus =
    store.viewsStatus === "loading"
      ? "loading"
      : store.viewsStatus === "error"
        ? "error"
        : cache?.status ?? (store.viewId ? "loading" : "ready");
  const error =
    store.viewsStatus === "error"
      ? store.viewsError
      : cache?.error ?? null;

  return {
    status,
    view,
    views: store.views,
    items: cache?.items ?? [],
    resumable: store.resumable,
    totalItems: cache?.total ?? 0,
    loadingMore: cache?.loadingMore ?? false,
    hasMore: Boolean(
      cache &&
        cache.status === "ready" &&
        cache.error === null &&
        cache.items.length < cache.total &&
        cache.items.length < LIBRARY_MAX_ITEMS,
    ),
    error,
    supportsTypeFilter: viewSupportsTypeFilter(view),
    resumeStatus: store.resumeStatus,
    resumeError: store.resumeError,
    selectView,
    loadMore,
    reload,
    retryResume,
    refreshResume,
  };
}

export function useEmbyDetail(
  itemId: string,
  seed: EmbyLibraryItem | null,
  enabled: boolean,
): EmbyDetailState {
  const desktop = hasTauriRuntime();
  const [item, setItem] = useState<EmbyLibraryItem | null>(seed);
  const [photos, setPhotos] = useState<Record<string, string>>({});
  const [loading, setLoading] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [reloadToken, setReloadToken] = useState(0);

  useEffect(() => {
    setItem(seed);
    setPhotos({});
    setError(null);

    if (!enabled || !itemId || !desktop) {
      setLoading(false);
      return;
    }

    let disposed = false;
    setLoading(true);

    void embyItem(itemId)
      .then(async (next) => {
        const decorated = await decorateItem(next, true, 600);
        if (disposed) return;
        setItem(decorated);
        setLoading(false);
        const urls = await personPhotoUrls(next.people);
        if (!disposed) setPhotos(urls);
      })
      .catch((reason: unknown) => {
        if (disposed) return;
        setError(describeEmbyError(reason, "无法加载条目详情。"));
        setLoading(false);
      });

    return () => {
      disposed = true;
    };
  }, [desktop, enabled, itemId, reloadToken, seed]);

  const reload = useCallback(() => setReloadToken((value) => value + 1), []);

  return { item, photos, loading, error, reload };
}

export interface EmbySimilarState {
  items: EmbyLibraryItem[];
}

/** "More like this". Best effort: a failure only leaves the row out. */
export function useEmbySimilar(itemId: string | null, enabled: boolean): EmbySimilarState {
  const desktop = hasTauriRuntime();
  const [items, setItems] = useState<EmbyLibraryItem[]>([]);

  useEffect(() => {
    setItems([]);
    if (!itemId || !enabled || !desktop) return;
    let disposed = false;
    void embySimilar(itemId, 12)
      .then((page) => Promise.all(page.items.map((item) => decorateItem(item, false))))
      .then((decorated) => {
        if (!disposed) setItems(decorated);
      })
      .catch((reason: unknown) => {
        console.warn("[emby] could not load similar items", reason);
      });
    return () => {
      disposed = true;
    };
  }, [desktop, enabled, itemId]);

  return { items };
}

export function useEmbyEpisodes(
  seriesId: string | null,
  enabled: boolean,
): EmbyEpisodesState {
  const desktop = hasTauriRuntime();
  const [items, setItems] = useState<EmbyLibraryItem[]>([]);
  const [loading, setLoading] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [reloadToken, setReloadToken] = useState(0);

  useEffect(() => {
    setItems([]);
    setError(null);

    if (!seriesId || !enabled || !desktop) {
      setLoading(false);
      return;
    }

    let disposed = false;
    setLoading(true);

    void embyItems({
      parentId: seriesId,
      includeItemTypes: "Episode",
      recursive: true,
      sortBy: "ParentIndexNumber,IndexNumber",
      limit: 500,
    })
      .then((page) => {
        if (disposed) return;
        setItems(page.items.map(plainLibraryItem));
        setLoading(false);
      })
      .catch((reason: unknown) => {
        if (disposed) return;
        setError(describeEmbyError(reason, "无法读取剧集列表。"));
        setLoading(false);
      });

    return () => {
      disposed = true;
    };
  }, [desktop, enabled, reloadToken, seriesId]);

  const reload = useCallback(() => setReloadToken((value) => value + 1), []);

  return { items, loading, error, reload };
}

export function usePlaybackLauncher(): PlaybackLauncher {
  const [pending, setPending] = useState(false);
  const [error, setError] = useState<string | null>(null);

  const launch = useCallback(async (itemId: string): Promise<EmbyPlaybackTarget | null> => {
    setPending(true);
    setError(null);
    try {
      const target = await embyPlaybackTarget(itemId);
      if (!target.url.trim()) {
        throw new Error("服务器没有返回可播放的直链。");
      }
      return target;
    } catch (reason) {
      setError(
        describeEmbyError(
          reason,
          "无法开始播放。该条目可能需要转码，当前不支持。",
        ),
      );
      return null;
    } finally {
      setPending(false);
    }
  }, []);

  const clearError = useCallback(() => setError(null), []);

  return { pending, error, launch, clearError };
}
