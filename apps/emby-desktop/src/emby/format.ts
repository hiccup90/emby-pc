import type { PlaybackMeta } from "../player/contract";
import type { EmbyItem } from "./contract";

const TICKS_PER_SECOND = 10_000_000;

export function ticksToSeconds(ticks: number | null | undefined): number {
  if (typeof ticks !== "number" || !Number.isFinite(ticks) || ticks <= 0) return 0;
  return ticks / TICKS_PER_SECOND;
}

export function formatRuntimeTicks(ticks: number | null | undefined): string {
  const totalMinutes = Math.round(ticksToSeconds(ticks) / 60);
  if (totalMinutes <= 0) return "时长未知";
  const hours = Math.floor(totalMinutes / 60);
  const minutes = totalMinutes % 60;
  if (hours <= 0) return `${minutes} 分钟`;
  return minutes > 0 ? `${hours} 小时 ${minutes} 分` : `${hours} 小时`;
}

export function itemProgress(item: EmbyItem): number {
  const position = item.userData?.playbackPositionTicks;
  const duration = item.runTimeTicks;
  if (
    typeof position !== "number" ||
    position <= 0 ||
    typeof duration !== "number" ||
    duration <= 0
  ) {
    return 0;
  }
  return Math.min(1, Math.max(0, position / duration));
}

/** Remaining play time for a Continue Watching card. */
export function formatRemainingTicks(item: EmbyItem): string | null {
  const duration = item.runTimeTicks;
  const position = item.userData?.playbackPositionTicks;
  if (typeof duration !== "number" || duration <= 0) return null;
  const remaining = duration - (typeof position === "number" ? position : 0);
  if (remaining <= 0) return null;
  return `剩余 ${formatRuntimeTicks(remaining)}`;
}

export function itemKindLabel(item: Pick<EmbyItem, "type" | "mediaType">): string {
  switch (item.type) {
    case "Movie":
      return "电影";
    case "Episode":
      return "单集";
    case "Series":
      return "剧集";
    case "Season":
      return "季";
    case "BoxSet":
      return "合集";
    case "MusicAlbum":
      return "音乐专辑";
    case "Audio":
      return "音频";
    case "Video":
      return "视频";
    default:
      return item.mediaType === "Video" ? "视频" : item.type || "条目";
  }
}

export function itemYearLabel(item: Pick<EmbyItem, "productionYear">): string | null {
  return typeof item.productionYear === "number" && item.productionYear > 0
    ? String(item.productionYear)
    : null;
}

/** "S1 · E3" (Plex style, no zero padding). */
export function itemEpisodeLabel(
  item: Pick<EmbyItem, "parentIndexNumber" | "indexNumber">,
): string | null {
  if (item.parentIndexNumber === null || item.indexNumber === null) return null;
  return `S${item.parentIndexNumber} · E${item.indexNumber}`;
}

/** Player title row for an episode, like Plex Web: series, then "S1 · E3 — name". */
export function episodePlaybackMeta(
  seriesName: string | null | undefined,
  item: Pick<EmbyItem, "name" | "parentIndexNumber" | "indexNumber">,
): PlaybackMeta {
  const label = itemEpisodeLabel(item);
  const name = item.name.trim();
  const series = seriesName?.trim();
  if (!series) return { title: name || label || "", subtitle: name ? label : null };
  const subtitle = [label, name].filter((part): part is string => Boolean(part)).join(" — ");
  return { title: series, subtitle: subtitle || null };
}

export function itemPlaybackMeta(item: EmbyItem): PlaybackMeta {
  return item.type === "Episode"
    ? episodePlaybackMeta(item.seriesName, item)
    : { title: item.name, subtitle: itemYearLabel(item) };
}
