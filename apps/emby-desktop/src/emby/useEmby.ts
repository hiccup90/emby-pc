import { useCallback, useEffect, useState } from "react";
import {
  describeEmbyError,
  embyImageUrl,
  embyItem,
  embyItems,
  embyLogin,
  embyLogout,
  embyPlaybackTarget,
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
  feature: EmbyLibraryItem | null;
  totalItems: number;
  error: string | null;
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

export interface PlaybackLauncher {
  pending: boolean;
  error: string | null;
  launch: (itemId: string) => Promise<EmbyPlaybackTarget | null>;
  clearError: () => void;
}

function emptyLibrary(): EmbyLibraryState {
  return {
    status: "idle",
    view: null,
    views: [],
    items: [],
    resumable: [],
    feature: null,
    totalItems: 0,
    error: null,
  };
}

function pickLibraryView(views: EmbyView[]): EmbyView | null {
  return (
    views.find((view) => view.collectionType?.toLowerCase() === "movies") ??
    views[0] ??
    null
  );
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
): EmbyLibraryState & { reload: () => void } {
  const desktop = hasTauriRuntime();
  const [state, setState] = useState<EmbyLibraryState>(emptyLibrary);
  const [reloadToken, setReloadToken] = useState(0);

  useEffect(() => {
    if (!enabled || !session || !desktop) {
      setState(emptyLibrary());
      return;
    }

    let disposed = false;
    setState((current) => ({ ...current, status: "loading", error: null }));

    const load = async () => {
      try {
        const views = await embyViews();
        if (disposed) return;

        const view = pickLibraryView(views);
        if (!view) {
          setState({
            ...emptyLibrary(),
            status: "ready",
            views,
          });
          return;
        }

        const [itemsResult, resumeResult] = await Promise.allSettled([
          embyItems({
            parentId: view.id,
            includeItemTypes: "Movie",
            recursive: true,
            sortBy: "DateCreated,SortName",
            limit: 60,
          }),
          embyItems({
            includeItemTypes: "Movie,Episode",
            recursive: true,
            filters: "IsResumable",
            limit: 12,
            sortBy: "DatePlayed",
          }),
        ]);
        if (disposed) return;

        if (itemsResult.status === "rejected") {
          throw itemsResult.reason;
        }

        const page = itemsResult.value;
        const items = await Promise.all(
          page.items.map((item) => decorateItem(item, false)),
        );
        if (disposed) return;

        let feature: EmbyLibraryItem | null = null;
        if (items[0]) {
          feature = await decorateItem(page.items[0], true);
          if (disposed) return;
        }

        let resumable: EmbyLibraryItem[] = [];
        if (resumeResult.status === "fulfilled") {
          resumable = await Promise.all(
            resumeResult.value.items.map((item) => decorateItem(item, false)),
          );
        } else {
          console.warn("[emby] continue-watching request failed", resumeResult.reason);
        }
        if (disposed) return;

        setState({
          status: "ready",
          view,
          views,
          items,
          resumable,
          feature,
          totalItems: page.totalRecordCount,
          error: null,
        });
      } catch (reason) {
        if (disposed) return;
        setState((current) => ({
          ...current,
          status: "error",
          error: describeEmbyError(reason, "无法加载媒体库，请重试。"),
        }));
      }
    };

    void load();
    return () => {
      disposed = true;
    };
  }, [desktop, enabled, reloadToken, session]);

  const reload = useCallback(() => setReloadToken((value) => value + 1), []);

  return { ...state, reload };
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
