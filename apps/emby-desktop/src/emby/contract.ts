// Wire contract shared with the Rust side. Keep these field names in sync with
// the `emby_*` commands; the Rust agent implements the other half in parallel.

export interface EmbySession {
  serverUrl: string;
  userId: string;
  userName: string;
  serverName: string;
  deviceId: string;
}

export interface EmbyView {
  id: string;
  name: string;
  collectionType: string | null;
}

export interface EmbyUserData {
  playbackPositionTicks: number | null;
  played: boolean;
  unplayedItemCount: number | null;
}

export interface EmbyItem {
  id: string;
  name: string;
  type: string;
  mediaType: string | null;
  productionYear: number | null;
  runTimeTicks: number | null;
  imageTags: Record<string, string>;
  backdropImageTags: string[];
  primaryImageAspectRatio: number | null;
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
  endDate: string | null;
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
