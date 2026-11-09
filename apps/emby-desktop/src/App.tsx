import { useCallback, useEffect, useRef, useState } from "react";
import type { EmbyPlaybackTarget } from "./emby/contract";
import { hasTauriRuntime } from "./emby/client";
import type { EmbyLibraryItem } from "./emby/useEmby";
import { useEmbyLibrary, useEmbySession } from "./emby/useEmby";
import DetailScreen from "./screens/DetailScreen";
import LibraryScreen from "./screens/LibraryScreen";
import LoginScreen from "./screens/LoginScreen";
import PlayerPage from "./player/PlayerPage";
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

type Route =
  | { screen: "login" }
  | { screen: "library" }
  | ({ screen: "detail" } & DetailLocation)
  | {
      screen: "player";
      playback: EmbyPlaybackTarget;
      title: string;
      returnTo: DetailLocation;
    };

function BootScreen() {
  return (
    <main className="boot-screen" role="status" aria-live="polite">
      <span className="boot-screen__brand">Emby PC</span>
      <span className="boot-screen__status">正在读取 Emby 会话…</span>
      <span className="boot-screen__bar" aria-hidden="true" />
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

  useEffect(() => {
    if (route.screen === "detail" || route.screen === "player") {
      window.scrollTo(0, 0);
    }
  }, [route]);

  const openItem = useCallback((item: EmbyLibraryItem) => {
    setRoute({ screen: "detail", itemId: item.id, seed: item });
  }, []);

  const openPlayer = useCallback(
    (playback: EmbyPlaybackTarget, title: string, returnTo: DetailLocation) => {
      setRoute({ screen: "player", playback, title, returnTo });
    },
    [],
  );

  const logout = useCallback(() => {
    void (async () => {
      await sessionController.signOut();
      setRoute({ screen: "login" });
    })();
  }, [sessionController]);

  if (desktop && (sessionController.checking || (sessionController.session && route.screen === "login"))) {
    return (
      <div className="app-shell app-shell--boot">
        <BootScreen />
      </div>
    );
  }

  return (
    <div className={`app-shell app-shell--${route.screen}`}>
      {route.screen === "login" && (
        <LoginScreen
          desktop={desktop}
          pending={sessionController.loginPending}
          loginError={sessionController.loginError}
          startupError={sessionController.startupError}
          onLogin={sessionController.signIn}
          onPreview={() => setRoute({ screen: "library" })}
          onClearError={sessionController.clearLoginError}
        />
      )}

      {route.screen === "library" && (
        <LibraryScreen
          session={sessionController.session}
          desktop={desktop}
          library={library}
          logoutPending={sessionController.logoutPending}
          onOpen={openItem}
          onLogout={logout}
        />
      )}

      {route.screen === "detail" && (
        <DetailScreen
          key={route.itemId}
          itemId={route.itemId}
          seed={route.seed}
          session={sessionController.session}
          desktop={desktop}
          logoutPending={sessionController.logoutPending}
          onBack={() => setRoute({ screen: "library" })}
          onPlay={(playback, title) =>
            openPlayer(playback, title, {
              itemId: route.itemId,
              seed: route.seed,
            })
          }
          onLogout={logout}
        />
      )}

      {route.screen === "player" && (
        <PlayerPage
          playback={route.playback}
          onBack={() =>
            setRoute({
              screen: "detail",
              itemId: route.returnTo.itemId,
              seed: route.returnTo.seed,
            })
          }
        />
      )}

    </div>
  );
}
