import { useCallback, useEffect, useState } from "react";
import type { EmbyPlaybackTarget } from "./emby/contract";
import { hasTauriRuntime } from "./emby/client";
import type { EmbyLibraryItem } from "./emby/useEmby";
import { useEmbyLibrary, useEmbySession } from "./emby/useEmby";
import DetailScreen from "./screens/DetailScreen";
import LibraryScreen from "./screens/LibraryScreen";
import LoginScreen from "./screens/LoginScreen";
import { BrandMark } from "./screens/shared";
import PlayerPage from "./player/PlayerPage";
import "./styles.css";

if (typeof document !== "undefined" && !document.querySelector('link[rel="icon"]')) {
  const icon = document.createElement("link");
  icon.rel = "icon";
  icon.type = "image/svg+xml";
  const svg = "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 32 32'><rect width='32' height='32' fill='#171a17'/><path d='M16 6 26 16 16 26 6 16Z' fill='none' stroke='#c58b65' stroke-width='2'/></svg>";
  icon.href = `data:image/svg+xml,${encodeURIComponent(svg)}`;
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
      <BrandMark />
      <span className="eyebrow">EMBY DESKTOP</span>
      <h1>正在读取 Emby 会话…</h1>
      <span className="boot-screen__line" aria-hidden="true" />
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

  useEffect(() => {
    if (sessionController.session && route.screen === "login") {
      setRoute({ screen: "library" });
    }
  }, [route.screen, sessionController.session]);

  useEffect(() => {
    if (route.screen === "detail" || route.screen === "player") {
      window.scrollTo(0, 0);
    }
  }, [route]);

  const openItem = useCallback((item: EmbyLibraryItem) => {
    setRoute({ screen: "detail", itemId: item.id, seed: item });
  }, []);

  const openRelated = useCallback((item: EmbyLibraryItem) => {
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
          libraryItems={library.items}
          session={sessionController.session}
          desktop={desktop}
          logoutPending={sessionController.logoutPending}
          onBack={() => setRoute({ screen: "library" })}
          onOpenRelated={openRelated}
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
          title={route.title}
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
