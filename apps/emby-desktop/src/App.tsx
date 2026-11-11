import { useCallback, useEffect, useLayoutEffect, useRef, useState } from "react";
import type { EmbyItem, EmbyPlaybackTarget } from "./emby/contract";
import { hasTauriRuntime } from "./emby/client";
import type {
  EmbyLibraryItem,
  EmbyLibraryQuery,
  EmbyLoadStatus,
} from "./emby/useEmby";
import {
  DEFAULT_LIBRARY_QUERY,
  useEmbyLibrary,
  useEmbySession,
  usePlaybackLauncher,
} from "./emby/useEmby";
import { describeEmbyError, embyItems } from "./emby/client";
import { decorateItems } from "./emby/useSearch";
import { episodePlaybackMeta, itemChapters, itemPlaybackMeta } from "./emby/format";
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
import { BrandMark, PageBackdrop } from "./screens/shared";
import PlayerPage from "./player/PlayerPage";
import type { PlaybackMeta } from "./player/contract";
import { DEFAULT_APP_SETTINGS, getSettings } from "./settings/tauri";
import type { AppSettings } from "./settings/tauri";
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
type DetailRoute = DetailLocation & { trail: DetailLocation[] };

type Route =
  | { screen: "login" }
  | { screen: "library" }
  | { screen: "home" }
  | { screen: "search" }
  | { screen: "settings" }
  | { screen: "actor"; personId: string; personName: string; from: DetailRoute }
  | ({ screen: "detail" } & DetailRoute)
  | {
      screen: "player";
      playback: EmbyPlaybackTarget;
      meta: PlaybackMeta;
      returnTo: DetailRoute;
      /** Series or movie id used to remember the chosen audio/subtitle track. */
      trackScope: string | null;
    };

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
  const library = useEmbyLibrary(session, Boolean(session), {
    query,
    pageSize: settings.pageSize,
  });
  const [route, setRoute] = useState<Route>({ screen: "login" });
  const autoPlay = settings.autoPlay;
  const previousScreenRef = useRef<Route["screen"] | null>(null);
  const contentRef = useRef<HTMLDivElement>(null);
  /** Library scroll offset to restore when coming back from a detail page. */
  const libraryScrollRef = useRef(0);
  const [backdrop, setBackdrop] = useState<string | null>(null);
  const { refreshResume } = library;

  // Read the persisted settings once; a failure keeps the defaults.
  useEffect(() => {
    let cancelled = false;
    void getSettings()
      .then((stored) => {
        if (!cancelled) setSettings(stored);
      })
      .catch((reason: unknown) => {
        console.warn("[settings] could not read the persisted settings", reason);
      });
    return () => {
      cancelled = true;
    };
  }, []);

  useEffect(() => {
    const previous = previousScreenRef.current;
    previousScreenRef.current = route.screen;
    if (
      route.screen === "library" &&
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
    if (route.screen === "library") {
      box.scrollTop = libraryScrollRef.current;
    } else if (route.screen === "detail") {
      box.scrollTop = 0;
    }
  }, [route]);

  // A new page size refetches page 0; the old scroll offset would land in the
  // middle of a differently-sized grid, so keep the user near the top.
  useEffect(() => {
    libraryScrollRef.current = 0;
  }, [settings.pageSize]);

  const goToLibrary = useCallback(() => setRoute({ screen: "library" }), []);

  const changeQuery = useCallback((next: EmbyLibraryQuery) => {
    libraryScrollRef.current = 0;
    contentRef.current?.scrollTo({ top: 0 });
    setQuery(next);
  }, []);

  const chooseView = useCallback(
    (viewId: string) => {
      libraryScrollRef.current = 0;
      library.selectView(viewId);
      setRoute({ screen: "library" });
      contentRef.current?.scrollTo({ top: 0 });
    },
    [library],
  );

  const openItem = useCallback((item: EmbyLibraryItem) => {
    libraryScrollRef.current = contentRef.current?.scrollTop ?? 0;
    setRoute({ screen: "detail", itemId: item.id, seed: item, trail: [] });
  }, []);

  // "Recently added" for the home screen (server-sorted, newest first). The row
  // publishes its own load state so a pending/failed request never looks empty.
  const [recent, setRecent] = useState<EmbyLibraryItem[]>([]);
  const [recentStatus, setRecentStatus] = useState<EmbyLoadStatus>("idle");
  const [recentError, setRecentError] = useState<string | null>(null);
  const [recentReloadToken, setRecentReloadToken] = useState(0);

  useEffect(() => {
    if (!session) {
      setRecent([]);
      setRecentStatus("idle");
      setRecentError(null);
      return undefined;
    }
    let cancelled = false;
    setRecentStatus("loading");
    setRecentError(null);
    void (async () => {
      try {
        const page = await embyItems({
          recursive: true,
          sortBy: "DateCreated",
          sortOrder: "Descending",
          limit: 24,
          includeItemTypes: "Movie,Series",
        });
        const decorated = await decorateItems(page.items);
        if (cancelled) return;
        setRecent(decorated);
        setRecentStatus("ready");
      } catch (reason) {
        if (cancelled) return;
        console.warn("[home] could not load recently added", reason);
        setRecent([]);
        setRecentError(describeEmbyError(reason, "无法读取最近添加的内容。"));
        setRecentStatus("error");
      }
    })();
    return () => {
      cancelled = true;
    };
  }, [recentReloadToken, session]);

  const retryRecent = useCallback(() => setRecentReloadToken((value) => value + 1), []);

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
              from: { itemId: current.itemId, seed: current.seed, trail: current.trail },
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
    }));
  }, []);

  const leaveDetail = useCallback(() => {
    setRoute((current) => {
      if (current.screen !== "detail" || current.trail.length === 0) return { screen: "library" };
      const previous = current.trail[current.trail.length - 1];
      return { screen: "detail", ...previous, trail: current.trail.slice(0, -1) };
    });
  }, []);

  const { launch } = usePlaybackLauncher();
  /** Episodes still to play: [0] is on screen, the rest auto-play in order. */
  const queueRef = useRef<EmbyItem[] | null>(null);

  // The single session-state transition: signed-in lands on home once the
  // login page is no longer current; signed-out always returns to login and
  // clears the episode queue (unmounting PlayerPage stops mpv).
  useLayoutEffect(() => {
    if (sessionState.status === "signedIn") {
      setRoute((current) => (current.screen === "login" ? { screen: "home" } : current));
    } else if (sessionState.status === "signedOut") {
      queueRef.current = null;
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
      queueRef.current = queue;
      setRoute({ screen: "player", playback, meta, returnTo, trackScope });
    },
    [],
  );

  /** Auto-play the next queued episode; false when the queue is exhausted. */
  const playNext = useCallback(async () => {
    let queue = queueRef.current;
    if (!queue || queue.length === 0) return false;
    if (queue.length < 2) {
      // Opened from continue-watching: only the current episode is queued, so
      // fetch the rest of the series on demand.
      const current = queue[0];
      if (current.type !== "Episode" || !current.seriesId) return false;
      try {
        const page = await embyItems({
          parentId: current.seriesId,
          includeItemTypes: "Episode",
          recursive: true,
          sortBy: "ParentIndexNumber,IndexNumber",
          limit: 500,
        });
        const index = page.items.findIndex((entry) => entry.id === current.id);
        if (index < 0 || index + 1 >= page.items.length) return false;
        queue = page.items.slice(index);
        queueRef.current = queue;
      } catch (reason) {
        console.warn("[player] could not load the next episode", reason);
        return false;
      }
    }
    if (!queue || queue.length < 2) return false;
    const next = queue[1];
    const target = await launch(next.id);
    if (!target) return false;
    queueRef.current = queue.slice(1);
    const meta =
      next.type === "Episode"
        ? { ...episodePlaybackMeta(next.seriesName, next), chapters: itemChapters(next) }
        : { ...itemPlaybackMeta(next), chapters: itemChapters(next) };
    setRoute((current) =>
      current.screen === "player"
        ? { ...current, playback: target, meta, trackScope: next.seriesId ?? next.id }
        : current,
    );
    return true;
  }, [launch]);

  /** Continue-watching cards start playback directly; the server supplies the resume point. */
  const resumeItem = useCallback(
    async (item: EmbyLibraryItem) => {
      const target = await launch(item.id);
      if (!target) return;
      const meta =
        item.type === "Episode"
          ? { ...episodePlaybackMeta(item.seriesName, item), chapters: itemChapters(item) }
          : { ...itemPlaybackMeta(item), chapters: itemChapters(item) };
      const returnTo: DetailRoute =
        item.type === "Episode" && item.seriesId
          ? { itemId: item.seriesId, seed: null, trail: [] }
          : { itemId: item.id, seed: item, trail: [] };
      openPlayer(target, meta, returnTo, [item], item.seriesId ?? item.id);
    },
    [launch, openPlayer],
  );

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
          onNext={autoPlay ? playNext : undefined}
          onBack={() => {
            queueRef.current = null;
            setRoute({
              screen: "detail",
              itemId: route.returnTo.itemId,
              seed: route.returnTo.seed,
              trail: route.returnTo.trail,
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
    <div className={`app-shell app-shell--${route.screen}`}>
      <TitleBar
        account={account}
        onBack={route.screen === "detail" ? leaveDetail : undefined}
        backLabel={route.screen === "detail" && route.trail.length > 0 ? "返回" : "返回媒体库"}
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
              resume={library.resumable}
              resumeStatus={library.resumeStatus}
              resumeError={library.resumeError}
              recent={recent}
              recentStatus={recentStatus}
              recentError={recentError}
              onOpen={openItem}
              onPlay={resumeItem}
              onSeeAll={goToLibrary}
              onRetryRecent={retryRecent}
              onRetryResume={library.retryResume}
            />
          )}

          {route.screen === "search" && (
            <SearchScreen onOpen={openItem} onPlay={resumeItem} />
          )}

          {route.screen === "settings" && (
            <SettingsScreen
              onBack={() => setRoute({ screen: "home" })}
              onSettingsChange={setSettings}
            />
          )}

          {route.screen === "actor" && (
            <ActorScreen
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
              onOpen={openItem}
              onPlay={resumeItem}
            />
          )}

          {route.screen === "detail" && (
            <DetailScreen
              key={route.itemId}
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
                  { itemId: route.itemId, seed: route.seed, trail: route.trail },
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
    </div>
  );
}
