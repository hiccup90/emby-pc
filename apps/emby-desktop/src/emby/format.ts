import type { PlaybackMeta } from "../player/contract";
import type { EmbyItem, EmbyMediaStream } from "./contract";

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


/** "2010年9月27日". Read from the string: Emby stores dates as UTC midnight,
 * so going through Date would shift the day in time zones west of UTC. */
export function formatPremiereDate(value: string | null | undefined): string | null {
  const match = value?.match(/^(\d{4})-(\d{2})-(\d{2})/);
  if (!match) return null;
  return `${Number(match[1])}年${Number(match[2])}月${Number(match[3])}日`;
}

export function formatCommunityRating(value: number | null | undefined): string | null {
  if (typeof value !== "number" || !Number.isFinite(value) || value <= 0) return null;
  return value.toFixed(1);
}

export function seriesStatusLabel(status: string | null | undefined): string | null {
  switch (status) {
    case "Continuing":
      return "连载中";
    case "Ended":
      return "已完结";
    default:
      return null;
  }
}

const LANGUAGE_NAMES: Record<string, string> = {
  chi: "中文",
  zho: "中文",
  chs: "简体中文",
  cht: "繁体中文",
  eng: "英语",
  jpn: "日语",
  kor: "韩语",
  fre: "法语",
  fra: "法语",
  ger: "德语",
  deu: "德语",
  spa: "西班牙语",
  rus: "俄语",
  ita: "意大利语",
  por: "葡萄牙语",
  tha: "泰语",
};

const CODEC_NAMES: Record<string, string> = {
  h264: "H.264",
  avc: "H.264",
  hevc: "HEVC",
  h265: "HEVC",
  av1: "AV1",
  vp9: "VP9",
  mpeg2video: "MPEG-2",
  aac: "AAC",
  ac3: "AC3",
  eac3: "E-AC3",
  dts: "DTS",
  truehd: "TrueHD",
  flac: "FLAC",
  opus: "Opus",
  mp3: "MP3",
  srt: "SRT",
  subrip: "SRT",
  ass: "ASS",
  ssa: "SSA",
  pgssub: "PGS",
  hdmv_pgs_subtitle: "PGS",
  dvdsub: "VobSub",
  dvd_subtitle: "VobSub",
  mov_text: "MOV Text",
  webvtt: "WebVTT",
};

function codecName(codec: string | null): string | null {
  if (!codec) return null;
  return CODEC_NAMES[codec.toLowerCase()] ?? codec.toUpperCase();
}

function channelLabel(channels: number | null): string | null {
  switch (channels) {
    case 1:
      return "单声道";
    case 2:
      return "立体声";
    case 6:
      return "5.1";
    case 8:
      return "7.1";
    default:
      return channels ? `${channels} 声道` : null;
  }
}

/** Width counts too: a letterboxed 1920x800 film is still 1080p. */
function resolutionLabel(width: number | null, height: number | null): string | null {
  const w = width ?? 0;
  const h = height ?? 0;
  if (w >= 3200 || h >= 2000) return "4K";
  if (w >= 1800 || h >= 1000) return "1080p";
  if (w >= 1200 || h >= 700) return "720p";
  if (h >= 560) return "576p";
  if (h >= 400) return "480p";
  return h > 0 ? `${h}p` : null;
}

function pickStream(streams: EmbyMediaStream[], kind: string) {
  const ofKind = streams.filter((stream) => stream.kind === kind);
  return { chosen: ofKind.find((stream) => stream.isDefault) ?? ofKind[0] ?? null, count: ofKind.length };
}

function streamLabel(stream: EmbyMediaStream): string {
  if (stream.displayTitle) return stream.displayTitle;
  const language = stream.language ? LANGUAGE_NAMES[stream.language.toLowerCase()] ?? stream.language : null;
  const channels = stream.kind === "Audio" ? channelLabel(stream.channels) : null;
  return [language, codecName(stream.codec), channels, stream.title].filter(Boolean).join(" ") || "未知";
}

function withCount(label: string, count: number): string {
  return count > 1 ? `${label}（共 ${count} 条）` : label;
}

/** "1080p (H.264)" for the default (or first) video stream. */
export function videoStreamSummary(streams: EmbyMediaStream[]): string | null {
  const { chosen } = pickStream(streams, "Video");
  if (!chosen) return null;
  const resolution = resolutionLabel(chosen.width, chosen.height);
  const codec = codecName(chosen.codec);
  if (resolution && codec) return `${resolution} (${codec})`;
  return resolution ?? codec ?? chosen.displayTitle;
}

export function audioStreamSummary(streams: EmbyMediaStream[]): string | null {
  const { chosen, count } = pickStream(streams, "Audio");
  return chosen ? withCount(streamLabel(chosen), count) : null;
}

/** Null without stream info; "无" when there is video but no subtitle. */
export function subtitleStreamSummary(streams: EmbyMediaStream[]): string | null {
  const { chosen, count } = pickStream(streams, "Subtitle");
  if (chosen) return withCount(streamLabel(chosen), count);
  return streams.some((stream) => stream.kind === "Video") ? "无" : null;
}
