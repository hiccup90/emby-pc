import { useEffect, useState } from "react";
import { embyImageUrl, embyPersonItems } from "../emby/client";
import type { EmbyLibraryItem } from "../emby/useEmby";
import { decorateItems } from "../emby/artwork";
import { useAsyncResource } from "../emby/useAsyncResource";
import { ErrorNotice, Icon, MediaArtwork, useEscape } from "./shared";
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

interface ActorWorks {
  items: EmbyLibraryItem[];
  totalItems: number;
}

export default function ActorScreen({ personId, personName, onOpen, onBack }: ActorScreenProps) {
  const [photoUrl, setPhotoUrl] = useState<string | null>(null);

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

  const works = useAsyncResource<ActorWorks>(
    async () => {
      const page = await embyPersonItems(personId, PERSON_ITEMS_LIMIT);
      const items = await decorateItems(page.items);
      return { items, totalItems: page.totalRecordCount };
    },
    `actor:${personId}`,
    { fallbackError: "无法读取该演员的作品。" },
  );

  useEscape(onBack);

  const items = works.data?.items ?? [];
  const totalItems = works.data?.totalItems ?? 0;
  const status: ActorStatus =
    works.status === "ready" ? (items.length === 0 ? "empty" : "ready") : works.status === "error" ? "error" : "loading";
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
            message={works.error ?? "无法读取该演员的作品。"}
            onRetry={works.reload}
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