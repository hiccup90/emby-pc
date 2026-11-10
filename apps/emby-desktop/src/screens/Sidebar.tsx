import type { EmbyView } from "../emby/contract";
import { collectionTypeOf } from "../emby/library";
import { Icon } from "./shared";
import type { IconName } from "./shared";

function viewIcon(view: EmbyView): IconName {
  switch (collectionTypeOf(view)) {
    case "movies":
      return "film";
    case "tvshows":
      return "tv";
    case "homevideos":
      return "camera";
    case "music":
      return "music";
    default:
      return "folder";
  }
}

interface SidebarProps {
  serverName: string | null;
  views: EmbyView[];
  activeViewId: string | null;
  loading: boolean;
  /** One short line when there is nothing to list (preview, error, empty). */
  status: string | null;
  onSelect: (viewId: string) => void;
}

/** Library navigation (Plex-style left rail). Collapses to icons in narrow windows. */
export default function Sidebar({
  serverName,
  views,
  activeViewId,
  loading,
  status,
  onSelect,
}: SidebarProps) {
  return (
    <nav className="sidebar" aria-label="媒体库">
      {serverName && (
        <p className="sidebar__heading" title={serverName}>
          <Icon name="server" size={14} />
          <span className="sidebar__label">{serverName}</span>
        </p>
      )}
      {views.length > 0 ? (
        <ul className="sidebar__list">
          {views.map((view) => {
            const active = view.id === activeViewId;
            return (
              <li key={view.id}>
                <button
                  type="button"
                  className={`sidebar__item${active ? " is-active" : ""}`}
                  aria-current={active ? "page" : undefined}
                  title={view.name}
                  onClick={() => onSelect(view.id)}
                >
                  <Icon name={viewIcon(view)} size={20} />
                  <span className="sidebar__label">{view.name}</span>
                </button>
              </li>
            );
          })}
        </ul>
      ) : (
        loading && (
          <div className="sidebar__skeleton" aria-hidden="true">
            <span />
            <span />
            <span />
          </div>
        )
      )}
      {status && (
        <p className="sidebar__status" role="status">
          {status}
        </p>
      )}
    </nav>
  );
}
