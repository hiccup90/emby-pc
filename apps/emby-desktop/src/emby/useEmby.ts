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
  embySession,
  embyViews,
  hasTauriRuntime,
} from "./client";
import type { EmbyCredentials } from "./client";
import type {
  EmbyItem,
  EmbyPlaybackTarget,
  EmbySession,
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
  selectView: (viewId: string) => void;
  loadMore: () => void;
  reload: () => void;
  refreshResume: () => void;
}

export interface EmbySessionController {
  session: EmbySession | null;
  checking: boolean;
  loginPending: boolean;
  logoutPending: boolean;
  loginError: string | null;
  startupError: string | null;
  signIn: (credentials: EmbyCredentials) => Promise<boolean>;
  signOut: () => Promise<void>;
  clearLoginError: () => void;
}

export interface EmbyDetailState {
  item: EmbyLibraryItem | null;
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
  cache: Record<string, ViewCache>;
  resumable: EmbyLibraryItem[];
  resumeStatus: EmbyLoadStatus;
}

function emptyStore(): LibraryStore {
  return {
    views: [],
    viewsStatus: "idle",
    viewsError: null,
    viewId: null,
    cache: {},
    resumable: [],
    resumeStatus: "idle",
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

function plainLibraryItem(item: EmbyItem): EmbyLibraryItem {
  return { ...item, posterUrl: null, backdropUrl: null };
}

export function useEmbySession(): EmbySessionController {
  const desktop = hasTauriRuntime();
  const [session, setSession] = useState<EmbySession | null>(null);
  const [checking, setChecking] = useState(desktop);
  const [loginPending, setLoginPending] = useState(false);
  const [logoutPending, setLogoutPending] = useState(false);
  const [loginError, setLoginError] = useState<string | null>(null);
  const [startupError, setStartupError] = useState<string | null>(null);

  useEffect(() => {
    if (!desktop) {
      setChecking(false);
      return;
    }

    let cancelled = false;
    setChecking(true);
    void embySession()
      .then((existing) => {
        if (!cancelled) setSession(existing);
      })
      .catch((reason: unknown) => {
        if (!cancelled) {
          setStartupError(
            describeEmbyError(reason, "无法读取已有 Emby 会话，请重新登录。"),
          );
        }
      })
      .finally(() => {
        if (!cancelled) setChecking(false);
      });

    return () => {
      cancelled = true;
    };
  }, [desktop]);

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
        setSession(next);
        setStartupError(null);
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
    try {
      if (hasTauriRuntime()) {
        await embyLogout();
      }
    } catch (reason) {
      console.warn("[emby] logout failed; clearing the local session anyway", reason);
    } finally {
      setSession(null);
      setLoginPending(false);
      setLoginError(null);
      setLogoutPending(false);
    }
  }, []);

  const clearLoginError = useCallback(() => setLoginError(null), []);

  return {
    session,
    checking,
    loginPending,
    logoutPending,
    loginError,
    startupError,
    signIn,
    signOut,
    clearLoginError,
  };
}

export function useEmbyLibrary(
  session: EmbySession | null,
  enabled: boolean,
): EmbyLibraryController {
  const desktop = hasTauriRuntime();
  const [store, setStore] = useState<LibraryStore>(emptyStore);
  const storeRef = useRef<LibraryStore>(store);
  const generationRef = useRef(0);
  const requestsRef = useRef<Set<string>>(new Set());
  const resumeRefreshRef = useRef(false);
  const [reloadToken, setReloadToken] = useState(0);

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
      const key = view.id;
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
        const includeItemTypes = includeItemTypesForView(view);
        const page = await embyItems({
          parentId: view.id,
          recursive: true,
          sortBy: "SortName",
          limit: LIBRARY_PAGE_SIZE,
          startIndex,
          ...(includeItemTypes ? { includeItemTypes } : {}),
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
    const loading = { ...emptyStore(), viewsStatus: "loading" as const, resumeStatus: "loading" as const };
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
        if (resumeResult.status === "fulfilled") {
          resumable = await Promise.all(
            resumeResult.value.items.map((item) => decorateItem(item, true, 640)),
          );
          if (generation !== generationRef.current) return;
        } else {
          console.warn("[emby] continue-watching request failed", resumeResult.reason);
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
          resumeStatus: "ready",
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

  const selectView = useCallback(
    (viewId: string) => {
      const current = storeRef.current;
      const view = current.views.find((candidate) => candidate.id === viewId);
      if (!view) return;
      if (current.viewId !== viewId) {
        updateStore((state) => ({ ...state, viewId }));
      }
      const cache = current.cache[viewId];
      if (!cache || cache.status === "error") {
        void fetchViewPage(view, "replace");
      }
    },
    [fetchViewPage, updateStore],
  );

  const loadMore = useCallback(() => {
    const current = storeRef.current;
    const view = current.views.find((candidate) => candidate.id === current.viewId);
    const cache = current.viewId ? current.cache[current.viewId] : undefined;
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
  const cache = store.viewId ? store.cache[store.viewId] : undefined;
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
    selectView,
    loadMore,
    reload,
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
  const [loading, setLoading] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [reloadToken, setReloadToken] = useState(0);

  useEffect(() => {
    setItem(seed);
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

  return { item, loading, error, reload };
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
