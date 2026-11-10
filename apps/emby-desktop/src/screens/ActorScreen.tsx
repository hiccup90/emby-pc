import { useCallback, useEffect, useState } from "react";
import { describeEmbyError, embyImageUrl, embyPersonItems } from "../emby/client";
import type { EmbyLibraryItem } from "../emby/useEmby";
import { decorateItems } from "../emby/useSearch";
import { ErrorNotice, Icon, MediaArtwork } from "./shared";
import { PosterCard } from "./cards";
import "./ActorScreen.css";

interface ActorScreenProps {
  personId: string;
  personName: string;
  /** Open the details page for one of the actor's works. */
  onOpen: (item: EmbyLibraryItem) => void;
  onBack: () => void;
}

type ActorStatus = "loading" | "ready" | "empty" | "error";

const PERSON_ITEMS_LIMIT = 120;
const SKELETON_COUNT = 14;

function ActorSkeleton() {
  return (
    <div className="loading-state" role="status" aria-live="polite">
      <span className="visually-hidden">正在读取演员作品…</span>
      <div className="poster-grid" aria-hidden="true">
        {Array.from({ length: SKELETON_COUNT }, (_, index) => (
          <span className="skeleton-card" key={index}>
            <span className="skeleton-block" />
            <span className="skeleton-line" />
            <span className="skeleton-line skeleton-line--short" />
          </span>
        ))}
      </div>
    </div>
  );
}

export default function ActorScreen({ personId, personName, onOpen, onBack }: ActorScreenProps) {
  const [items, setItems] = useState<EmbyLibraryItem[]>([]);
  const [totalItems, setTotalItems] = useState(0);
  const [status, setStatus] = useState<ActorStatus>("loading");
  const [error, setError] = useState<string | null>(null);
  const [photoUrl, setPhotoUrl] = useState<string | null>(null);
  const [reloadToken, setReloadToken] = useState(0);

  useEffect(() => {
    let cancelled = false;
    setPhotoUrl(null);
    void embyImageUrl({ itemId: personId, imageType: "Primary", maxWidth: 240 })
      .then((url) => {
        if (!cancelled && url.trim()) setPhotoUrl(url);
      })
      .catch(() => {
        // A missing portrait is fine; MediaArtwork falls back to the name.
      });
    return () => {
      cancelled = true;
    };
  }, [personId]);

  useEffect(() => {
    let cancelled = false;
    setItems([]);
    setTotalItems(0);
    setStatus("loading");
    setError(null);

    void (async () => {
      try {
        const page = await embyPersonItems(personId, PERSON_ITEMS_LIMIT);
        if (cancelled) return;
        const decorated = await decorateItems(page.items);
        if (cancelled) return;
        setItems(decorated);
        setTotalItems(page.totalRecordCount);
        setStatus(decorated.length === 0 ? "empty" : "ready");
      } catch (reason) {
        if (cancelled) return;
        setError(describeEmbyError(reason, "无法读取该演员的作品。"));
        setStatus("error");
      }
    })();

    return () => {
      cancelled = true;
    };
  }, [personId, reloadToken]);

  useEffect(() => {
    const onKeyDown = (event: KeyboardEvent) => {
      if (event.key === "Escape" && !event.defaultPrevented) {
        event.preventDefault();
        onBack();
      }
    };
    window.addEventListener("keydown", onKeyDown);
    return () => window.removeEventListener("keydown", onKeyDown);
  }, [onBack]);

  const retry = useCallback(() => setReloadToken((value) => value + 1), []);
  const count = totalItems > 0 ? totalItems : items.length;

  return (
    <main className="page actor-page">
      <header className="actor-header">
        <button type="button" className="actor-header__back" onClick={onBack} aria-label="返回媒体库">
          <Icon name="arrow-left" size={18} />
        </button>
        <span className="actor-header__photo" aria-hidden="true">
          <MediaArtwork src={photoUrl} alt="" title={personName} loading="eager" />
        </span>
        <div className="actor-header__text">
          <p className="actor-header__eyebrow">演员</p>
          <h1 className="actor-header__name">{personName}</h1>
          {status === "ready" && <p className="actor-header__count">{count} 部作品</p>}
        </div>
      </header>

      {status === "loading" ? (
        <ActorSkeleton />
      ) : status === "error" ? (
        <div className="page-message">
          <ErrorNotice
            message={error ?? "无法读取该演员的作品。"}
            onRetry={retry}
            retryLabel="重新读取"
          />
        </div>
      ) : status === "empty" ? (
        <p className="empty-state">没有找到 {personName} 的作品。</p>
      ) : (
        <section aria-label={`${personName} 的作品`}>
          <div className="poster-grid">
            {items.map((item) => (
              <PosterCard key={item.id} item={item} landscape={false} onSelect={onOpen} />
            ))}
          </div>
        </section>
      )}
    </main>
  );
}
