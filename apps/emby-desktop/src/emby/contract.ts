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
}

export interface EmbyItemsPage {
  items: EmbyItem[];
  totalRecordCount: number;
  startIndex: number;
}

export interface EmbyPlaybackTarget {
  itemId: string;
  url: string;
  playSessionId: string | null;
  mediaSourceId: string | null;
  startSeconds: number;
}
