// Wire contract shared with the Rust side. Keep these field names in sync with
// the `emby_*` commands; the Rust agent implements the other half in parallel.

export interface EmbySession {
  serverUrl: string;
  userId: string;
  userName: string;
  serverName: string;
  deviceId: string;
}

export type EmbySessionStatus = "signedIn" | "signedOut";

/** Machine-readable reasons shared by boot results and command errors. */
export type EmbySessionReason =
  | "none"
  | "token_rejected"
  | "unreachable"
  | "server_error"
  | "storage_error"
  | "invalid_credentials"
  | "invalid_request";

export interface EmbySessionBoot {
  status: EmbySessionStatus;
  reason: EmbySessionReason;
  session?: EmbySession | null;
  lastServer?: string | null;
  lastUserName?: string | null;
}

export interface EmbyCommandError {
  reason: EmbySessionReason;
  message: string;
}

export interface EmbySessionEvent {
  status: EmbySessionStatus;
  reason: EmbySessionReason;
  message: string;
}

/** Event emitted when a running command gets a 401 (task-21 listens for it). */
export const EMBY_SESSION_EVENT = "emby-session";

export interface EmbyView {
  id: string;
  name: string;
  collectionType: string | null;
}

export interface EmbyUserData {
  playbackPositionTicks: number | null;
  played: boolean;
  favorite: boolean;
  unplayedItemCount: number | null;
}

export interface EmbyItem {
  id: string;
  name: string;
  type: string;
  mediaType: string | null;
  productionYear: number | null;
  /** Server-side creation time (ISO 8601); list/detail responses supply it. */
  dateCreated?: string | null;
  runTimeTicks: number | null;
  imageTags: Record<string, string>;
  backdropImageTags: string[];
  userData: EmbyUserData;
  seriesId: string | null;
  seriesName: string | null;
  indexNumber: number | null;
  parentIndexNumber: number | null;
  overview: string | null;
  genres: string[];
  // Detail data: only `emby_item` fills these; list items have them empty.
  people: EmbyPerson[];
  studios: string[];
  officialRating: string | null;
  communityRating: number | null;
  tagline: string | null;
  premiereDate: string | null;
  /** Series status: "Continuing" / "Ended". */
  status: string | null;
  mediaStreams: EmbyMediaStream[];
  chapters: EmbyChapter[];
}

export interface EmbyChapter {
  startSeconds: number;
  name: string;
}

export interface EmbyPerson {
  id: string | null;
  name: string;
  /** Character name for actors; often empty. */
  role: string | null;
  /** "Actor", "Director", "Writer", "GuestStar", ... */
  kind: string | null;
  primaryImageTag: string | null;
}

export interface EmbyMediaStream {
  /** "Video" | "Audio" | "Subtitle" */
  kind: string;
  index: number | null;
  codec: string | null;
  language: string | null;
  title: string | null;
  displayTitle: string | null;
  width: number | null;
  height: number | null;
  channels: number | null;
  isDefault: boolean;
  isForced: boolean;
  isExternal: boolean;
}

export interface EmbyItemsPage {
  items: EmbyItem[];
  totalRecordCount: number;
  startIndex: number;
}

/** Direction accepted by Emby's `SortOrder` query parameter. */
export type EmbySortOrder = "Ascending" | "Descending";

/** Query for `emby_items` (`GET /Users/{userId}/Items`). */
export interface EmbyItemsRequest {
  parentId?: string;
  startIndex?: number;
  limit?: number;
  searchTerm?: string;
  includeItemTypes?: string;
  recursive?: boolean;
  sortBy?: string;
  /** Defaults to `Ascending` on the Rust side. */
  sortOrder?: EmbySortOrder;
  /** Comma-separated Emby `Filters` values, e.g. `IsUnplayed`. */
  filters?: string;
  /** Comma-separated genre names (`Genres` query parameter). */
  genres?: string;
}

/** Query for `emby_person_items` (`GET /Persons/{personId}/Items`). */
export interface EmbyPersonItemsRequest {
  personId: string;
  limit?: number;
}

/** Payload for `emby_set_played`. */
export interface EmbySetPlayedRequest {
  itemId: string;
  played: boolean;
}

/** Payload for `emby_set_favorite`. */
export interface EmbySetFavoriteRequest {
  itemId: string;
  favorite: boolean;
}

export interface EmbyResumeRequest {
  limit?: number;
}

export interface EmbyPlaybackTarget {
  itemId: string;
  url: string;
  playSessionId: string | null;
  mediaSourceId: string | null;
  startSeconds: number;
}
