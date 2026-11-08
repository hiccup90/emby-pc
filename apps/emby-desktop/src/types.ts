export type Screen = "login" | "home" | "detail" | "player";

export type MediaKind = "电影" | "剧集";

export type ArtKey = "north" | "orbit" | "harbor" | "forest" | "city" | "desert";

export interface DemoMedia {
  id: string;
  title: string;
  subtitle: string;
  kind: MediaKind;
  year: number;
  genres: string[];
  runtime: string;
  durationSeconds: number;
  rating: string;
  synopsis: string;
  artKey: ArtKey;
  progress: number;
  detailNote: string;
}

export type TrackMenu = "audio" | "subtitle" | null;
