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
    default:
      return item.mediaType === "Video" ? "视频" : item.type || "条目";
  }
}

export function itemYearLabel(item: Pick<EmbyItem, "productionYear">): string | null {
  return typeof item.productionYear === "number" && item.productionYear > 0
    ? String(item.productionYear)
    : null;
}

export function itemEpisodeLabel(
  item: Pick<EmbyItem, "parentIndexNumber" | "indexNumber">,
): string | null {
  if (item.parentIndexNumber === null || item.indexNumber === null) return null;
  return `S${String(item.parentIndexNumber).padStart(2, "0")} · E${String(item.indexNumber).padStart(2, "0")}`;
}

export function userInitial(name: string): string {
  const trimmed = name.trim();
  if (!trimmed) return "·";
  return Array.from(trimmed)[0] ?? "·";
}

export function compactUrl(rawUrl: string | null | undefined): string {
  if (!rawUrl) return "";
  try {
    const url = new URL(rawUrl);
    const path = url.pathname.length > 44 ? `${url.pathname.slice(0, 41)}…` : url.pathname;
    return `${url.origin}${path}`;
  } catch {
    return rawUrl.split("?")[0] ?? rawUrl;
  }
}
