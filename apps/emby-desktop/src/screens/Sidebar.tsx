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
  views: EmbyView[];
  activeViewId: string | null;
  loading: boolean;
  /** One short line when there is nothing to list (preview, error, empty). */
  status: string | null;
  onSelect: (viewId: string) => void;
  activeNav?: "home" | "search" | "settings" | null;
  onHome?: () => void;
  onSearch?: () => void;
  onSettings?: () => void;
}

/** Library navigation (Plex-style left rail). Collapses to icons in narrow windows. */
export default function Sidebar({
  views,
  activeViewId,
  loading,
  status,
  onSelect,
  activeNav = null,
  onHome,
  onSearch,
  onSettings,
}: SidebarProps) {
  const navItem = (
    key: "home" | "search" | "settings",
    label: string,
    icon: IconName,
    onClick?: () => void,
  ) =>
    onClick ? (
      <li>
        <button
          type="button"
          className={`sidebar__item${activeNav === key ? " is-active" : ""}`}
          aria-current={activeNav === key ? "page" : undefined}
          title={label}
          onClick={onClick}
        >
          <Icon name={icon} size={22} />
          <span className="sidebar__label">{label}</span>
        </button>
      </li>
    ) : null;

  return (
    <nav className="sidebar" aria-label="媒体库">
      <ul className="sidebar__list">
        {navItem("home", "首页", "home", onHome)}
        {navItem("search", "搜索", "search", onSearch)}
      </ul>
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
                  <Icon name={viewIcon(view)} size={22} />
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
      {onSettings && (
        <ul className="sidebar__list sidebar__list--footer">
          {navItem("settings", "设置", "settings", onSettings)}
        </ul>
      )}
    </nav>
  );
}
