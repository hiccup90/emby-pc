import { useCallback, useEffect, useLayoutEffect, useRef, useState } from "react";
import type { EmbyPlaybackTarget } from "./emby/contract";
import { hasTauriRuntime } from "./emby/client";
import type { EmbyLibraryItem } from "./emby/useEmby";
import { useEmbyLibrary, useEmbySession } from "./emby/useEmby";
import DetailScreen from "./screens/DetailScreen";
import LibraryScreen from "./screens/LibraryScreen";
import LoginScreen from "./screens/LoginScreen";
import Sidebar from "./screens/Sidebar";
import TitleBar from "./screens/TitleBar";
import type { TitleBarAccount } from "./screens/TitleBar";
import { BrandMark, PageBackdrop } from "./screens/shared";
import PlayerPage from "./player/PlayerPage";
import type { PlaybackMeta } from "./player/contract";
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
}

/** A detail page plus the detail pages it was opened from ("more like this"). */
type DetailRoute = DetailLocation & { trail: DetailLocation[] };

type Route =
  | { screen: "login" }
  | { screen: "library" }
  | ({ screen: "detail" } & DetailRoute)
  | {
      screen: "player";
      playback: EmbyPlaybackTarget;
      meta: PlaybackMeta;
      returnTo: DetailRoute;
    };

function BootScreen() {
  return (
    <main className="boot-screen" role="status" aria-live="polite">
      <BrandMark large />
      <span className="spinner" aria-hidden="true" />
      <span className="boot-screen__status">正在读取 Emby 会话…</span>
    </main>
  );
}

export default function App() {
  const desktop = hasTauriRuntime();
  const sessionController = useEmbySession();
  const library = useEmbyLibrary(
    sessionController.session,
    Boolean(sessionController.session),
  );
  const [route, setRoute] = useState<Route>({ screen: "login" });
  const previousScreenRef = useRef<Route["screen"] | null>(null);
  const contentRef = useRef<HTMLDivElement>(null);
  /** Library scroll offset to restore when coming back from a detail page. */
  const libraryScrollRef = useRef(0);
  const [backdrop, setBackdrop] = useState<string | null>(null);
  const { refreshResume } = library;

  useEffect(() => {
    if (sessionController.session && route.screen === "login") {
      setRoute({ screen: "library" });
    }
  }, [route.screen, sessionController.session]);

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

  const goToLibrary = useCallback(() => setRoute({ screen: "library" }), []);

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

  const openPlayer = useCallback(
    (playback: EmbyPlaybackTarget, meta: PlaybackMeta, returnTo: DetailRoute) => {
      setRoute({ screen: "player", playback, meta, returnTo });
    },
    [],
  );

  const logout = useCallback(() => {
    void (async () => {
      await sessionController.signOut();
      libraryScrollRef.current = 0;
      setRoute({ screen: "login" });
    })();
  }, [sessionController]);

  const booting =
    desktop &&
    (sessionController.checking ||
      (sessionController.session && route.screen === "login"));

  if (booting) {
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
          onBack={() =>
            setRoute({
              screen: "detail",
              itemId: route.returnTo.itemId,
              seed: route.returnTo.seed,
              trail: route.returnTo.trail,
            })
          }
        />
      </div>
    );
  }

  const session = sessionController.session;
  const browsing = route.screen === "library" || route.screen === "detail";
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
          />
        )}
        <div className="app-content" ref={contentRef}>
          {route.screen === "login" && (
            <LoginScreen
              desktop={desktop}
              pending={sessionController.loginPending}
              loginError={sessionController.loginError}
              startupError={sessionController.startupError}
              onLogin={sessionController.signIn}
              onPreview={goToLibrary}
              onClearError={sessionController.clearLoginError}
            />
          )}

          {route.screen === "library" && (
            <LibraryScreen session={session} desktop={desktop} library={library} onOpen={openItem} />
          )}

          {route.screen === "detail" && (
            <DetailScreen
              key={route.itemId}
              itemId={route.itemId}
              seed={route.seed}
              session={session}
              desktop={desktop}
              onBack={leaveDetail}
              onOpenItem={openRelated}
              onPlay={(playback, meta) =>
                openPlayer(playback, meta, {
                  itemId: route.itemId,
                  seed: route.seed,
                  trail: route.trail,
                })
              }
              onBackdrop={setBackdrop}
            />
          )}
        </div>
      </div>
    </div>
  );
}
