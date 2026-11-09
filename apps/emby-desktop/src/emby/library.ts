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

export function includeItemTypesForCollectionType(
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

/** Movies first, otherwise the first view returned by the server. */
export function pickDefaultView(views: EmbyView[]): EmbyView | null {
  return (
    views.find((view) => collectionTypeOf(view) === "movies") ?? views[0] ?? null
  );
}

export function isPlayableItemType(type: string | null | undefined): boolean {
  return type === "Movie" || type === "Episode" || type === "Video";
}
