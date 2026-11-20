import { useCallback, useEffect, useLayoutEffect, useMemo, useRef, useState } from "react";
import type { EmbyItem, EmbyPlaybackTarget } from "./emby/contract";
import { describeEmbyError, embyDeleteItems, hasTauriRuntime } from "./emby/client";
import type {
  EmbyLibraryItem,
  EmbyLibraryQuery,
} from "./emby/useEmby";
import {
  DEFAULT_LIBRARY_QUERY,
  useEmbyLibrary,
  useEmbySession,
  usePlaybackLauncher,
  fetchEpisodeRun,
  clearEpisodeRunCache,
} from "./emby/useEmby";
import { resolveHomeViews } from "./emby/library";
import { itemStartSeconds, playbackMeta } from "./emby/format";
import ActorScreen from "./screens/ActorScreen";
import DetailScreen from "./screens/DetailScreen";
import HomeScreen from "./screens/HomeScreen";
import SearchScreen from "./screens/SearchScreen";
import SettingsScreen from "./screens/SettingsScreen";
import LibraryScreen from "./screens/LibraryScreen";
import LoginScreen from "./screens/LoginScreen";
import Sidebar from "./screens/Sidebar";
import TitleBar from "./screens/TitleBar";
import type { TitleBarAccount } from "./screens/TitleBar";
import {
  DeleteDialog,
  IdentifyDialog,
  ItemActionContext,
  ItemContextMenu,
  SelectionBar,
  IDENTIFY_ITEM_TYPES,
} from "./screens/ItemActions";
import type { ItemActionContextValue, ItemMenuState } from "./screens/ItemActions";
import { BrandMark, ErrorNotice, PageBackdrop } from "./screens/shared";
import PlayerPage from "./player/PlayerPage";
import type { PlaybackMeta } from "./player/contract";
import { DEFAULT_APP_SETTINGS, getSettings, saveSettings } from "./settings/tauri";
import type { AppSettings, LibraryViewMode } from "./settings/tauri";
import "./styles.css";

if (typeof document !== "undefined" && !document.querySelector('link[rel="icon"]')) {
  const icon = document.createElement("link");
  icon.rel = "icon";
  icon.href = "data:,";
  document.head.append(icon);
}

interface DetailLocation {
  itemId: string;
  seed: EmbyLibraryItem | null;
  /** Episode to reveal (season + scroll + highlight) after returning from playback. */
  focusEpisodeId?: string | null;
}

/** A detail page plus the detail pages it was opened from ("more like this"). */
type DetailRoute = DetailLocation & {
  trail: DetailLocation[];
  /** Where "back" goes once the trail is empty — the page the user came from. */
  origin: DetailOrigin;
};

/**
 * The page a detail view was opened from. "Back" used to always mean the
 * library, which threw away the search results (and the actor page) the user
 * was browsing; it now returns to the page that actually opened the detail.
 */
type DetailOrigin =
  | { screen: "home" }
  | { screen: "search" }
  | { screen: "library" }
  | ActorRoute;

type ActorRoute = {
  screen: "actor";
  personId: string;
  personName: string;
  /** The detail page the actor page was opened from. */
  from: DetailRoute;
};

type Route =
  | { screen: "login" }
  | { screen: "library" }
  | { screen: "home" }
  | { screen: "search" }
  | { screen: "settings" }
  | ActorRoute
  | ({ screen: "detail" } & DetailRoute)
  | {
      screen: "player";
      playback: EmbyPlaybackTarget;
      meta: PlaybackMeta;
      returnTo: DetailRoute;
      /** Series or movie id used to remember the chosen audio/subtitle track. */
      trackScope: string | null;
    };

/** The page a detail view is being opened from, so "back" can return there. */
function originOf(route: Route): DetailOrigin {
  switch (route.screen) {
    case "actor":
      return route;
    case "home":
      return { screen: "home" };
    case "search":
      return { screen: "search" };
    default:
      return { screen: "library" };
  }
}

function BootScreen() {
  return (
    <main className="boot-screen" role="status" aria-live="polite">
      <BrandMark large />
      <span className="spinner" aria-hidden="true" />
      <span className="boot-screen__status">正在恢复登录…</span>
    </main>
  );
}

export default function App() {
  const desktop = hasTauriRuntime();
  const sessionController = useEmbySession();
  const sessionState = sessionController.state;
  const session = sessionController.session;
  /** Persisted settings; pageSize drives the library page size, autoPlay the queue. */
  const [settings, setSettings] = useState<AppSettings>(() => ({ ...DEFAULT_APP_SETTINGS }));
  const [query, setQuery] = useState<EmbyLibraryQuery>(DEFAULT_LIBRARY_QUERY);
  const [settingsReady, setSettingsReady] = useState(false);
  const library = useEmbyLibrary(session, Boolean(session) && settingsReady, {
    query,
    pageSize: settings.pageSize,
    storedQueries: settings.libraryQueries,
    onRestoredQuery: setQuery,
  });
  const [route, setRoute] = useState<Route>({ screen: "login" });
  /**
   * Search box text, owned here so a detail (or player) round-trip comes back to
   * the same results. SearchScreen keys its request on this value.
   */
  const [searchQuery, setSearchQuery] = useState("");
  /** Latest route, readable from callbacks that must not re-create on navigation. */
  const routeRef = useRef(route);
  routeRef.current = route;
  const autoPlay = settings.autoPlay;
  const previousScreenRef = useRef<Route["screen"] | null>(null);
  const contentRef = useRef<HTMLDivElement>(null);
  /** Library scroll offset to restore when coming back from a detail page. */
  const libraryScrollRef = useRef(0);
  const [backdrop, setBackdrop] = useState<string | null>(null);
  const { refreshResume } = library;
  /** Card right-click / multi-select state; only the current page may keep it. */
  const [selection, setSelection] = useState<{ active: boolean; ids: Set<string> }>({
    active: false,
    ids: new Set(),
  });
  const [selectedItems, setSelectedItems] = useState<Map<string, EmbyLibraryItem>>(new Map());
  const [itemMenu, setItemMenu] = useState<ItemMenuState | null>(null);
  const [deleteRequest, setDeleteRequest] = useState<EmbyLibraryItem[] | null>(null);
  const [identifyItem, setIdentifyItem] = useState<EmbyLibraryItem | null>(null);
  const [deleting, setDeleting] = useState(false);
  const deletingRef = useRef(false);
  const [deleteError, setDeleteError] = useState<string | null>(null);
  /** Bumped after delete/identify so the page that owns the cards re-reads it. */
  const [dataRevision, setDataRevision] = useState(0);

  // Read the persisted settings once; a failure keeps the defaults.
  useEffect(() => {
    let cancelled = false;
    void getSettings()
      .then((stored) => {
        if (!cancelled) setSettings(stored);
      })
      .catch((reason: unknown) => {
        console.warn("[settings] could not read the persisted settings", reason);
      })
      .finally(() => {
        // Even a failed read leaves the defaults usable; only then may the
        // library boot, so it never fetches page 0 with the default query and
        // immediately refetches with the stored one.
        if (!cancelled) setSettingsReady(true);
      });
    return () => {
      cancelled = true;
    };
  }, []);

  useEffect(() => {
    const previous = previousScreenRef.current;
    previousScreenRef.current = route.screen;
    if (
      (route.screen === "library" || route.screen === "home") &&
      (previous === "detail" || previous === "player")
    ) {
      refreshResume();
    }
  }, [refreshResume, route.screen]);

  // Pages scroll inside `.app-content` (title bar and sidebar stay put): detail
  // pages start at the top, the library returns to where the user left it.
  useLayoutEffect(() => {
    const box = contentRef.current;
    if (!box) return;
    box.scrollTop = route.screen === "library" ? libraryScrollRef.current : 0;
  }, [route]);

  // A new page size refetches page 0; the old scroll offset would land in the
  // middle of a differently-sized grid, so keep the user near the top.
  useEffect(() => {
    libraryScrollRef.current = 0;
  }, [settings.pageSize]);

  const goToLibrary = useCallback(() => setRoute({ screen: "library" }), []);

  /**
   * Open a detail page. The current page is remembered as its origin, so
   * "back" returns to the search results or the actor page instead of always
   * dumping the user in the library.
   */
  const openItem = useCallback(
    (item: EmbyLibraryItem) => {
      libraryScrollRef.current = contentRef.current?.scrollTop ?? 0;
      setRoute((current) => ({
        screen: "detail",
        itemId: item.id,
        seed: item,
        trail: [],
        origin: originOf(current),
      }));
    },
    [],
  );

  const changeQuery = useCallback(
    (next: EmbyLibraryQuery) => {
      libraryScrollRef.current = 0;
      contentRef.current?.scrollTo({ top: 0 });
      setQuery(next);
      const viewId = library.view?.id;
      if (!viewId) return;
      const nextSettings: AppSettings = {
        ...settings,
        libraryQueries: { ...settings.libraryQueries, [viewId]: next },
      };
      setSettings(nextSettings);
      if (!desktop) return;
      void saveSettings(nextSettings).catch((reason: unknown) => {
        console.warn("[settings] could not persist the library query", reason);
      });
    },
    [desktop, library.view?.id, settings],
  );

  /** Persist the card shape for the library currently on screen. */
  const changeLibraryViewMode = useCallback(
    (mode: LibraryViewMode) => {
      const viewId = library.view?.id;
      if (!viewId) return;
      const next: AppSettings = {
        ...settings,
        libraryViewModes: { ...settings.libraryViewModes, [viewId]: mode },
      };
      setSettings(next);
      if (!desktop) return;
      void saveSettings(next).catch((reason: unknown) => {
        console.warn("[settings] could not persist the library view mode", reason);
      });
    },
    [desktop, library.view?.id, settings],
  );

  const chooseView = useCallback(
    (viewId: string) => {
      libraryScrollRef.current = 0;
      const nextQuery = settings.libraryQueries[viewId] ?? DEFAULT_LIBRARY_QUERY;
      setQuery(nextQuery);
      library.selectView(viewId, nextQuery);
      setRoute({ screen: "library" });
      contentRef.current?.scrollTo({ top: 0 });
    },
    [library, settings.libraryQueries],
  );

  // The home page shows one fixed-size "recently added" row per selected
  // library; `null` means the movie/TV defaults from the user's Emby views.
  const homeViews = useMemo(
    () => resolveHomeViews(library.views, settings.homeViewIds),
    [library.views, settings.homeViewIds],
  );

  const clearSelection = useCallback(() => {
    setSelection({ active: false, ids: new Set() });
    setSelectedItems(new Map());
  }, []);

  const toggleSelection = useCallback((item: EmbyLibraryItem) => {
    setSelectedItems((current) => {
      const next = new Map(current);
      if (next.has(item.id)) next.delete(item.id);
      else next.set(item.id, item);
      return next;
    });
    setSelection((current) => {
      const ids = new Set(current.ids);
      if (ids.has(item.id)) ids.delete(item.id);
      else ids.add(item.id);
      // Like the official selection panel, deselecting the last item exits
      // selection mode instead of leaving an empty bar on screen.
      return { active: ids.size > 0, ids };
    });
  }, []);

  const startSelection = useCallback((item: EmbyLibraryItem) => {
    setItemMenu(null);
    setSelectedItems((current) => new Map(current).set(item.id, item));
    setSelection((current) => {
      const ids = new Set(current.active ? current.ids : []);
      ids.add(item.id);
      return { active: true, ids };
    });
  }, []);

  const openItemMenu = useCallback((item: EmbyLibraryItem, x: number, y: number) => {
    setItemMenu({ item, x, y });
  }, []);

  const selectedList = useMemo(() => [...selectedItems.values()], [selectedItems]);
  const selectionCanIdentify =
    selectedList.length === 1 && IDENTIFY_ITEM_TYPES.has(selectedList[0].type);

  // A different screen or library view is a different selection scope. This
  // also closes a stale menu/dialog when the sidebar navigates away.
  const actionScope =
    route.screen === "library"
      ? [
          "library",
          library.view?.id ?? "",
          query.sortBy,
          query.sortOrder,
          query.itemType,
          String(query.unplayedOnly),
          String(settings.pageSize),
        ].join(":")
      : route.screen === "detail"
        ? `detail:${route.itemId}`
        : route.screen === "actor"
          ? `actor:${route.personId}`
          : route.screen === "search"
            ? `search:${searchQuery}`
            : route.screen;
  useEffect(() => {
    setSelection({ active: false, ids: new Set() });
    setSelectedItems(new Map());
    setItemMenu(null);
    setDeleteRequest(null);
    setIdentifyItem(null);
    // `route` changes even when the screen name stays the same (e.g. clicking
    // home while already there); that still starts a new selection scope.
  }, [actionScope, route]);

  // Escape cancels selection before a page-level handler (detail "back") can
  // see it. Menus/dialogs have their own capture handlers.
  useEffect(() => {
    if (!selection.active || itemMenu || deleteRequest || identifyItem) return undefined;
    const onKeyDown = (event: KeyboardEvent) => {
      if (event.key !== "Escape") return;
      event.preventDefault();
      event.stopPropagation();
      clearSelection();
    };
    document.addEventListener("keydown", onKeyDown, true);
    return () => document.removeEventListener("keydown", onKeyDown, true);
  }, [clearSelection, deleteRequest, identifyItem, itemMenu, selection.active]);

  const itemActions = useMemo<ItemActionContextValue>(
    () => ({
      selection: { active: selection.active, ids: selection.ids, toggle: toggleSelection },
      openMenu: openItemMenu,
    }),
    [openItemMenu, selection.active, selection.ids, toggleSelection],
  );

  const openActor = useCallback(
    (person: { id: string | null; name: string }) => {
      const personId = person.id;
      if (!personId) return;
      setRoute((current) =>
        current.screen === "detail"
          ? {
              screen: "actor",
              personId,
              personName: person.name,
              from: {
                itemId: current.itemId,
                seed: current.seed,
                trail: current.trail,
                origin: current.origin,
              },
            }
          : current,
      );
    },
    [],
  );

  /** "More like this": back returns to the current detail page. */
  const openRelated = useCallback((item: EmbyLibraryItem) => {
    setRoute((current) => ({
      screen: "detail",
      itemId: item.id,
      seed: item,
      trail:
        current.screen === "detail"
          ? [...current.trail, { itemId: current.itemId, seed: current.seed }]
          : [],
      // "More like this" does not change where the browsing started.
      origin: current.screen === "detail" ? current.origin : originOf(current),
    }));
  }, []);

  const leaveDetail = useCallback(() => {
    setRoute((current) => {
      if (current.screen !== "detail") return { screen: "library" };
      if (current.trail.length > 0) {
        const previous = current.trail[current.trail.length - 1];
        return {
          screen: "detail",
          ...previous,
          trail: current.trail.slice(0, -1),
          origin: current.origin,
        };
      }
      // Empty trail: back to the page this detail was opened from.
      return current.origin;
    });
  }, []);

  const requestDelete = useCallback((items: EmbyLibraryItem[]) => {
    if (items.length === 0) return;
    setItemMenu(null);
    setDeleteError(null);
    setDeleteRequest(items);
  }, []);

  const confirmDelete = useCallback(async () => {
    if (!deleteRequest || deletingRef.current) return;
    deletingRef.current = true;
    const ids = deleteRequest.map((item) => item.id);
    setDeleting(true);
    setDeleteError(null);
    try {
      await embyDeleteItems(ids);
      setDeleteRequest(null);
      clearSelection();
      setDataRevision((value) => value + 1);
      if (routeRef.current.screen === "library") library.reload();
      refreshResume();
      if (routeRef.current.screen === "detail" && ids.includes(routeRef.current.itemId)) {
        leaveDetail();
      }
    } catch (reason: unknown) {
      setDeleteError(describeEmbyError(reason, "删除失败，请稍后重试。"));
    } finally {
      deletingRef.current = false;
      setDeleting(false);
    }
  }, [clearSelection, deleteRequest, leaveDetail, library, refreshResume]);

  const openIdentify = useCallback((item: EmbyLibraryItem) => {
    setItemMenu(null);
    setIdentifyItem(item);
  }, []);

  const finishIdentify = useCallback(() => {
    setIdentifyItem(null);
    clearSelection();
    setDataRevision((value) => value + 1);
    if (routeRef.current.screen === "library") library.reload();
    refreshResume();
  }, [clearSelection, library, refreshResume]);

  const {
    launch,
    error: playbackError,
    clearError: clearPlaybackError,
  } = usePlaybackLauncher();
  /** Item whose launch failed; the banner's retry re-runs exactly that attempt. */
  const failedPlayItemRef = useRef<EmbyLibraryItem | null>(null);

  // A new page is a new intent: the previous playback failure is stale. Without
  // this the banner would outlive the click that caused it.
  useEffect(() => {
    clearPlaybackError();
  }, [clearPlaybackError, route.screen]);

  /**
   * The whole ordered episode run of the series on screen, fetched once per
   * series and shared by auto-play, the previous-episode action and the
   * previous/next availability the controls show. One request per series, no
   * matter how many episodes the user walks through.
   */
  /** The item actually on screen in the player (auto-play and step both update it). */
  const playingItemRef = useRef<EmbyItem | null>(null);
  /** One episode switch at a time: a double click must not skip two episodes. */
  const episodeStepPendingRef = useRef(false);
  /**
   * Ordered episode ids of the series on screen; null for anything that is not
   * an episode. The player page turns this into the previous/next availability
   * it broadcasts, so the controls never query the server.
   */
  const [episodeRunIds, setEpisodeRunIds] = useState<string[] | null>(null);

  // The single session-state transition: signed-in lands on home once the
  // login page is no longer current; signed-out always returns to login and
  // clears the episode queue (unmounting PlayerPage stops mpv).
  useLayoutEffect(() => {
    if (sessionState.status === "signedIn") {
      setRoute((current) => (current.screen === "login" ? { screen: "home" } : current));
    } else if (sessionState.status === "signedOut") {
      clearEpisodeRunCache();
      playingItemRef.current = null;
      setEpisodeRunIds(null);
      setRoute({ screen: "login" });
    }
  }, [sessionState.status]);

  const openPlayer = useCallback(
    (
      playback: EmbyPlaybackTarget,
      meta: PlaybackMeta,
      returnTo: DetailRoute,
      queue: EmbyItem[] | null = null,
      trackScope: string | null = null,
    ) => {
      // The queue is a forward slice (the detail page hands over the episodes
      // after the clicked one), so it only identifies the item on screen; the
      // full run is resolved from the series once, see loadEpisodeRun.
      playingItemRef.current = queue?.[0] ?? null;
      setRoute({ screen: "player", playback, meta, returnTo, trackScope });
    },
    [],
  );

  /**
   * The whole ordered episode run of one series, fetched at most once per
   * series and shared by auto-play, the previous-episode action and the
   * previous/next availability the controls show. Same request the auto-play
   * path always made; nothing new is queried.
   */
  const loadEpisodeRun = useCallback(
    async (item: EmbyItem): Promise<EmbyItem[] | null> => {
      if (item.type !== "Episode" || !item.seriesId) return null;
      try {
        return await fetchEpisodeRun(item.seriesId);
      } catch (reason) {
        console.warn("[player] could not load the episode list", reason);
        return null;
      }
    },
    [],
  );

  /**
   * Move `delta` episodes inside the current run and really start that file.
   * False (and nothing changes) at either end of the run, on a launch failure,
   * or when the item on screen is not an episode.
   */
  const stepEpisode = useCallback(
    async (delta: number): Promise<boolean> => {
      const currentItem = playingItemRef.current;
      if (!currentItem || episodeStepPendingRef.current) return false;
      episodeStepPendingRef.current = true;
      try {
        const run = await loadEpisodeRun(currentItem);
        if (!run) return false;
        const index = run.findIndex((entry) => entry.id === currentItem.id);
        const targetIndex = index + delta;
        if (index < 0 || targetIndex < 0 || targetIndex >= run.length) return false;
        const next = run[targetIndex];
        const target = await launch(next.id, itemStartSeconds(next));
        if (!target) return false;
        playingItemRef.current = next;
        const meta = playbackMeta(next, target.chapters);
        setRoute((current) =>
          current.screen === "player"
            ? { ...current, playback: target, meta, trackScope: next.seriesId ?? next.id }
            : current,
        );
        return true;
      } finally {
        episodeStepPendingRef.current = false;
      }
    },
    [launch, loadEpisodeRun],
  );

  /** Auto-play the next queued episode; false when the run is exhausted. */
  const playNext = useCallback(() => stepEpisode(1), [stepEpisode]);
  /** Explicit "previous episode" from the controls. */
  const playPrevious = useCallback(() => stepEpisode(-1), [stepEpisode]);

  /**
   * Previous/next availability for the controls. Recomputed when the item on
   * screen changes; the episode run is fetched once per series and cached, so
   * walking through episodes costs no extra requests.
   */
  const playingItemId = route.screen === "player" ? route.playback.itemId : null;

  useEffect(() => {
    if (!playingItemId) {
      setEpisodeRunIds(null);
      return undefined;
    }
    const item = playingItemRef.current;
    // A movie (or a route the ref does not describe yet) gets no episode run,
    // so the player page broadcasts no neighbours and the controls render
    // neither transport button.
    if (!item || item.id !== playingItemId || item.type !== "Episode" || !item.seriesId) {
      setEpisodeRunIds(null);
      return undefined;
    }
    let cancelled = false;
    void loadEpisodeRun(item).then((run) => {
      if (cancelled) return;
      setEpisodeRunIds(run ? run.map((entry) => entry.id) : null);
    });
    return () => {
      cancelled = true;
    };
  }, [loadEpisodeRun, playingItemId]);

  /** Continue-watching cards start playback directly; the server supplies the resume point. */
  const resumeItem = useCallback(
    async (item: EmbyLibraryItem) => {
      failedPlayItemRef.current = item;
      const target = await launch(item.id, itemStartSeconds(item));
      // `launch` reports its own failure through the launcher's error state,
      // which is rendered above the content instead of being swallowed: a
      // failed "continue watching" click must never look like a dead button.
      if (!target) return;
      failedPlayItemRef.current = null;
      const meta = playbackMeta(item, target.chapters);
      const current = routeRef.current;
      const returnTo: DetailRoute =
        item.type === "Episode" && item.seriesId
          ? { itemId: item.seriesId, seed: null, trail: [], origin: originOf(current) }
          : { itemId: item.id, seed: item, trail: [], origin: originOf(current) };
      openPlayer(target, meta, returnTo, [item], item.seriesId ?? item.id);
    },
    [launch, openPlayer],
  );

  /** Retry the exact launch that failed (the banner's only action). */
  const retryPlayback = useCallback(() => {
    const item = failedPlayItemRef.current;
    if (!item) return;
    clearPlaybackError();
    void resumeItem(item);
  }, [clearPlaybackError, resumeItem]);

  const logout = useCallback(() => {
    void (async () => {
      await sessionController.signOut();
      libraryScrollRef.current = 0;
    })();
  }, [sessionController]);

  if (sessionState.status === "booting") {
    return (
      <div className="app-shell app-shell--boot">
        <TitleBar />
        <div className="app-body">
          <div className="app-content">
            <BootScreen />
          </div>
        </div>
      </div>
    );
  }

  if (route.screen === "player") {
    return (
      <div className="app-shell app-shell--player">
        <PlayerPage
          playback={route.playback}
          meta={route.meta}
          trackScope={route.trackScope}
          onNext={playNext}
          onPrevious={playPrevious}
          // The browsing shell renders this banner; the player branch returns
          // PlayerPage alone, so episode stepping has to hand it over or a
          // failed launch during a step would show nothing at all.
          launchError={playbackError}
          autoPlayNext={autoPlay}
          skipIntro={settings.skipIntro}
          episodeRun={episodeRunIds}
          onBack={() => {
            clearEpisodeRunCache();
            playingItemRef.current = null;
            setEpisodeRunIds(null);
            setRoute({
              screen: "detail",
              itemId: route.returnTo.itemId,
              seed: route.returnTo.seed,
              trail: route.returnTo.trail,
              origin: route.returnTo.origin,
              // route.playback is the item actually on screen, so auto-play's
              // next episode is the one revealed when the user goes back.
              focusEpisodeId: route.playback.itemId,
            });
          }}
        />
      </div>
    );
  }

  const browsing =
    route.screen === "library" ||
    route.screen === "detail" ||
    route.screen === "home" ||
    route.screen === "search" ||
    route.screen === "settings" ||
    route.screen === "actor";
  const account: TitleBarAccount | null = browsing
    ? {
        userName: session?.userName ?? null,
        serverName: session?.serverName ?? null,
        serverUrl: session?.serverUrl ?? null,
        logoutPending: sessionController.logoutPending,
        onLogout: logout,
      }
    : null;

  const previewMode = !session || !desktop;
  const sidebarLoading = !previewMode && library.views.length === 0 && library.status === "loading";
  const sidebarStatus =
    library.views.length > 0
      ? null
      : previewMode
        ? "预览模式：未连接 Emby"
        : library.status === "error"
          ? "媒体库读取失败"
          : library.status === "ready"
            ? "没有可用的媒体库"
            : null;

  return (
    <ItemActionContext.Provider value={itemActions}>
    <div className={`app-shell app-shell--${route.screen}`}>
      <TitleBar
        account={account}
        onBack={route.screen === "detail" ? leaveDetail : undefined}
        backLabel="返回"
      />
      <div className="app-body">
        {route.screen === "detail" && backdrop && <PageBackdrop key={backdrop} src={backdrop} />}
        {browsing && (
          <Sidebar
            views={library.views}
            activeViewId={library.view?.id ?? null}
            loading={sidebarLoading}
            status={sidebarStatus}
            onSelect={chooseView}
            activeNav={
              route.screen === "home"
                ? "home"
                : route.screen === "search"
                  ? "search"
                  : route.screen === "settings"
                    ? "settings"
                    : null
            }
            onHome={() => setRoute({ screen: "home" })}
            onSearch={() => setRoute({ screen: "search" })}
            onSettings={() => setRoute({ screen: "settings" })}
          />
        )}
        <div className="app-content" ref={contentRef}>
          {playbackError && (
            <div className="app-content__notice" style={{ padding: "16px 40px 0" }}>
              <ErrorNotice message={playbackError} onRetry={retryPlayback} />
            </div>
          )}
          {route.screen === "login" && (
            <LoginScreen
              desktop={desktop}
              pending={sessionController.loginPending}
              loginError={sessionController.loginError}
              reason={sessionState.status === "signedOut" ? sessionState.reason : "none"}
              lastServer={sessionState.status === "signedOut" ? sessionState.lastServer : null}
              lastUserName={sessionState.status === "signedOut" ? sessionState.lastUserName : null}
              onRetry={() => void sessionController.retryBoot()}
              onLogin={sessionController.signIn}
              onPreview={goToLibrary}
              onClearError={sessionController.clearLoginError}
            />
          )}

          {route.screen === "home" && (
            <HomeScreen
              refreshKey={dataRevision}
              resume={library.resumable}
              resumeStatus={library.resumeStatus}
              resumeError={library.resumeError}
              views={homeViews}
              userId={session?.userId ?? ""}
              onOpen={openItem}
              onPlay={resumeItem}
              onSeeAll={goToLibrary}
              onRetryResume={library.retryResume}
            />
          )}

          {route.screen === "search" && (
            <SearchScreen
              refreshKey={dataRevision}
              onOpen={openItem}
              onPlay={resumeItem}
              initialQuery={searchQuery}
              onQueryChange={setSearchQuery}
            />
          )}

          {route.screen === "settings" && (
            <SettingsScreen
              views={library.views}
              onBack={() => setRoute({ screen: "home" })}
              onSettingsChange={setSettings}
            />
          )}

          {route.screen === "actor" && (
            <ActorScreen
              key={`${route.personId}:${dataRevision}`}
              personId={route.personId}
              personName={route.personName}
              onOpen={openItem}
              onBack={() => setRoute({ screen: "detail", ...route.from })}
            />
          )}

          {route.screen === "library" && (
            <LibraryScreen
              session={session}
              desktop={desktop}
              library={library}
              query={query}
              onQueryChange={changeQuery}
              viewModes={settings.libraryViewModes}
              onViewModeChange={changeLibraryViewMode}
              onOpen={openItem}
            />
          )}

          {route.screen === "detail" && (
            <DetailScreen
              key={`${route.itemId}:${dataRevision}`}
              itemId={route.itemId}
              seed={route.seed}
              focusEpisodeId={route.focusEpisodeId ?? null}
              session={session}
              desktop={desktop}
              onBack={leaveDetail}
              onOpenItem={openRelated}
              onPlay={(playback, meta, queue, trackScope) =>
                openPlayer(
                  playback,
                  meta,
                  {
                    itemId: route.itemId,
                    seed: route.seed,
                    trail: route.trail,
                    origin: route.origin,
                  },
                  queue,
                  trackScope,
                )
              }
              onBackdrop={setBackdrop}
              onOpenPerson={openActor}
            />
          )}
        </div>
      </div>
      {itemMenu && (
        <ItemContextMenu
          menu={itemMenu}
          canIdentify={IDENTIFY_ITEM_TYPES.has(itemMenu.item.type)}
          onClose={() => setItemMenu(null)}
          onStartSelection={startSelection}
          onDelete={(item) => requestDelete([item])}
          onIdentify={openIdentify}
        />
      )}
      {selection.active && (
        <SelectionBar
          count={selectedList.length}
          canIdentify={selectionCanIdentify}
          onDelete={() => requestDelete(selectedList)}
          onIdentify={() => selectedList[0] && openIdentify(selectedList[0])}
          onCancel={clearSelection}
        />
      )}
      {deleteRequest && (
        <DeleteDialog
          items={deleteRequest}
          busy={deleting}
          error={deleteError}
          onCancel={() => {
            if (!deleting) {
              setDeleteRequest(null);
              setDeleteError(null);
            }
          }}
          onConfirm={() => void confirmDelete()}
        />
      )}
      {identifyItem && (
        <IdentifyDialog
          item={identifyItem}
          onClose={() => setIdentifyItem(null)}
          onApplied={finishIdentify}
        />
      )}
    </div>
    </ItemActionContext.Provider>
  );
}
