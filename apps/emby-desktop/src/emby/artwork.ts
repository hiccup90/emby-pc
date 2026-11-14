import { embyImageUrl } from "./client";
import type { EmbyItem, EmbyPerson } from "./contract";

export type ArtworkKind = "Primary" | "Backdrop" | "Logo";

type ArtworkSource = Pick<EmbyItem, "id" | "imageTags" | "backdropImageTags">;

/** Resolve an Emby image URL through the Tauri bridge; null when there is no tag. */
export async function safeImageUrl(
  item: ArtworkSource,
  imageType: ArtworkKind,
  maxWidth: number,
): Promise<string | null> {
  const tag =
    imageType === "Primary"
      ? item.imageTags.Primary
      : imageType === "Backdrop"
        ? item.backdropImageTags[0]
        : item.imageTags.Logo;
  // No tag means the server has no such image: never ask for a URL.
  if (!tag) return null;

  try {
    const url = await embyImageUrl({ itemId: item.id, imageType, maxWidth, tag });
    return url.trim() ? url : null;
  } catch (reason) {
    console.warn(`[emby] could not build ${imageType} image URL`, reason);
    return null;
  }
}

export type DecoratedItem<T extends EmbyItem = EmbyItem> = T & {
  posterUrl: string | null;
  backdropUrl: string | null;
};

/** Decorate one raw item with resolved poster/backdrop URLs. */
export async function decorateItem<T extends EmbyItem>(
  item: T,
  includeBackdrop: boolean,
  posterWidth = 400,
): Promise<DecoratedItem<T>> {
  const [posterUrl, backdropUrl] = await Promise.all([
    safeImageUrl(item, "Primary", posterWidth),
    includeBackdrop ? safeImageUrl(item, "Backdrop", 1280) : Promise.resolve(null),
  ]);
  return { ...item, posterUrl, backdropUrl };
}

/** Decorate a list of raw items (search results, actor works, home rows). */
export async function decorateItems<T extends EmbyItem>(
  items: T[],
  withBackdrop = false,
): Promise<DecoratedItem<T>[]> {
  return Promise.all(items.map((item) => decorateItem(item, withBackdrop)));
}

/** One image URL per cast/crew member that has a primary image. */
export async function personPhotoUrls(people: EmbyPerson[]): Promise<Record<string, string>> {
  const withPhoto = people.filter((person) => person.id && person.primaryImageTag).slice(0, 40);
  const entries = await Promise.all(
    withPhoto.map(async (person): Promise<[string, string] | null> => {
      const id = person.id as string;
      const tag = person.primaryImageTag as string;
      try {
        const url = await embyImageUrl({
          itemId: id,
          imageType: "Primary",
          maxWidth: 240,
          tag,
        });
        return url.trim() ? [id, url] : null;
      } catch (reason) {
        console.warn("[emby] could not build a person image URL", reason);
        return null;
      }
    }),
  );
  return Object.fromEntries(entries.filter((entry): entry is [string, string] => entry !== null));
}
