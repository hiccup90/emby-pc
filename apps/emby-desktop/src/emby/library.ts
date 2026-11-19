import type { EmbyView } from "./contract";

/** Page size for one library request. */
export const LIBRARY_PAGE_SIZE = 120;
/** Hard cap for one library tab, as agreed with the product spec. */
export const LIBRARY_MAX_ITEMS = 480;
/** Continue-watching cards stay between six and eight items. */
export const RESUME_LIMIT = 8;

export type LibraryCollectionType =
  | "movies"
  | "tvshows"
  | "homevideos"
  | "music"
  | "unknown";

export function collectionTypeOf(
  view: Pick<EmbyView, "collectionType"> | null | undefined,
): LibraryCollectionType {
  const value = view?.collectionType?.trim().toLowerCase();
  switch (value) {
    case "movies":
    case "tvshows":
    case "homevideos":
    case "music":
      return value;
    default:
      return "unknown";
  }
}

function includeItemTypesForCollectionType(
  collectionType: LibraryCollectionType,
): string | undefined {
  switch (collectionType) {
    case "movies":
      return "Movie";
    case "tvshows":
      return "Series";
    case "homevideos":
      return "Video";
    case "music":
      return "MusicAlbum";
    default:
      return undefined;
  }
}

export function includeItemTypesForView(view: EmbyView): string | undefined {
  return includeItemTypesForCollectionType(collectionTypeOf(view));
}

/** The home page starts with the movie and TV libraries, in sidebar order. */
export function defaultHomeViewIds(views: EmbyView[]): string[] {
  return views
    .filter((view) => {
      const type = collectionTypeOf(view);
      return type === "movies" || type === "tvshows";
    })
    .map((view) => view.id);
}

/** Resolve persisted home ids to existing views, preserving the saved order. */
export function resolveHomeViews(
  views: EmbyView[],
  selectedIds: string[] | null,
): EmbyView[] {
  const byId = new Map(views.map((view) => [view.id, view]));
  const ordered: EmbyView[] = [];
  const seen = new Set<string>();
  for (const id of selectedIds ?? defaultHomeViewIds(views)) {
    const view = byId.get(id);
    if (view && !seen.has(id)) {
      seen.add(id);
      ordered.push(view);
    }
  }
  return ordered;
}

/** Movies first, otherwise the first view returned by the server. */
export function pickDefaultView(views: EmbyView[]): EmbyView | null {
  return (
    views.find((view) => collectionTypeOf(view) === "movies") ?? views[0] ?? null
  );
}

export function isPlayableItemType(type: string | null | undefined): boolean {
  return type === "Movie" || type === "Episode" || type === "Video";
}