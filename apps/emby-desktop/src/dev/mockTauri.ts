/**
 * Dev-only Tauri runtime mock for `pnpm dev:mock` (vite --mode mock).
 *
 * Runs the main window in a plain browser against a fictional Emby library, so
 * the UI can be reviewed and screenshotted without Windows, Rust or a server.
 * main.tsx imports this module only when `import.meta.env.MODE === "mock"`;
 * Vite turns that check into a constant, so production builds drop it.
 *
 * Scenario switches (query string):
 *   ?session=none   start signed out (login screen); the password "wrong" fails
 *   ?state=loading  library requests never finish (skeletons)
 *   ?state=error    reading the library list fails
 *   ?state=empty    libraries exist but are empty
 *   ?resume=none    no "continue watching" row
 *   ?posters=none   no artwork anywhere (fallback tiles)
 */
import { mockIPC, mockWindows } from "@tauri-apps/api/mocks";
import type {
  EmbyItem,
  EmbyMediaStream,
  EmbyPerson,
  EmbyItemsPage,
  EmbyPlaybackTarget,
  EmbySession,
  EmbyView,
} from "../emby/contract";
import type { OpenRequest, PlayerSnapshot, PlayerTrack } from "../player/contract";

const TICKS = 10_000_000;

const params = new URLSearchParams(window.location.search);
const scenario = {
  signedOut: params.get("session") === "none",
  state: params.get("state"),
  noResume: params.get("resume") === "none",
  noArt: params.get("posters") === "none",
};

// ---- fictional catalogue ----------------------------------------------------

const OVERVIEWS = [
  "一座常年被雾气笼罩的港口城市里，一名即将退休的警探被卷入一桩二十年前的旧案。随着线索一点点浮出水面，他发现自己也是这个故事的一部分。",
  "两个素不相识的人在一列夜车上相遇。天亮之前，他们必须决定是否说出各自隐藏多年的秘密。",
  "一支科考队在极地失去了与外界的联系，他们在冰原深处找到了一座不该存在的建筑。",
  "小镇上的夏天总是很长。十七岁的少年在那个夏天学会了告别，也第一次知道了父亲的过去。",
  "一位年轻的天文学家收到了一段来自深空的信号，信号里藏着一个只有她能读懂的名字。",
  "为了找回失踪的妹妹，她回到离开十年的故乡，却发现所有人都在刻意回避那一夜发生的事。",
];

interface Seed {
  title: string;
  year: number;
  minutes: number;
  genres: string[];
}

const MOVIES: Seed[] = [
  ["雾港", 2021, 118, ["悬疑", "剧情"]],
  ["长夜将尽", 2019, 104, ["剧情"]],
  ["北方以北", 2022, 126, ["冒险", "剧情"]],
  ["失重的夏天", 2020, 97, ["爱情", "青春"]],
  ["第七个房间", 2018, 112, ["惊悚", "悬疑"]],
  ["银河边缘", 2023, 141, ["科幻", "冒险"]],
  ["无声的证人", 2017, 109, ["犯罪", "悬疑"]],
  ["海上灯塔", 2016, 95, ["剧情", "家庭"]],
  ["白夜行者", 2021, 121, ["动作", "犯罪"]],
  ["千里之外", 2015, 133, ["战争", "剧情"]],
  ["黑匣子", 2020, 101, ["惊悚"]],
  ["雨季不再来", 2014, 99, ["爱情"]],
  ["星际信使", 2024, 148, ["科幻"]],
  ["迷城", 2019, 116, ["悬疑", "动作"]],
  ["归途", 2022, 107, ["剧情", "公路"]],
  ["野火", 2023, 113, ["灾难", "动作"]],
  ["风起长安", 2021, 138, ["古装", "动作"]],
  ["冰原", 2018, 102, ["冒险"]],
  ["最后的列车", 2020, 119, ["科幻", "惊悚"]],
  ["镜中人", 2017, 98, ["悬疑"]],
  ["深海之下", 2022, 124, ["科幻", "灾难"]],
  ["午夜电台", 2019, 93, ["剧情"]],
  ["远山的回声", 2016, 111, ["剧情", "家庭"]],
  ["The Long Quiet", 2021, 115, ["Drama"]],
  ["Glass Harbor", 2018, 103, ["Thriller"]],
  ["Northbound", 2023, 122, ["Adventure"]],
  ["Static Bloom", 2020, 96, ["Sci-Fi"]],
  ["Low Tide", 2019, 108, ["Drama", "Mystery"]],
  ["Second Sun", 2024, 131, ["Sci-Fi"]],
  ["纸上城市", 2022, 100, ["喜剧"]],
].map(([title, year, minutes, genres]) => ({ title, year, minutes, genres }) as Seed);

const DOCS: Seed[] = [
  ["山河之间", 2021, 92, ["纪录片", "自然"]],
  ["城市的呼吸", 2019, 78, ["纪录片"]],
  ["深海纪行", 2022, 88, ["纪录片", "自然"]],
  ["候鸟", 2020, 70, ["纪录片"]],
  ["手艺人", 2018, 64, ["纪录片", "人文"]],
  ["沙漠之光", 2023, 81, ["纪录片"]],
  ["雨林", 2017, 95, ["纪录片", "自然"]],
  ["极光", 2024, 59, ["纪录片"]],
].map(([title, year, minutes, genres]) => ({ title, year, minutes, genres }) as Seed);

const HOME_VIDEOS: Seed[] = [
  ["2024 春节", 2024, 18, []],
  ["海边日落", 2023, 6, []],
  ["毕业典礼", 2022, 42, []],
  ["生日聚会", 2023, 25, []],
  ["旅行 · 京都", 2024, 33, []],
  ["猫咪日常", 2024, 9, []],
].map(([title, year, minutes, genres]) => ({ title, year, minutes, genres }) as Seed);

const SERIES: Array<Seed & { seasons: number[] }> = [
  ["长安夜话", 2021, 46, ["古装", "悬疑"], [10, 8]],
  ["边境线", 2019, 52, ["犯罪", "剧情"], [8]],
  ["第九号站台", 2022, 44, ["科幻"], [6, 6, 8]],
  ["灯塔守望者", 2020, 48, ["剧情"], [10]],
  ["晨雾", 2023, 42, ["爱情", "都市"], [12]],
  ["迷雾档案", 2018, 50, ["悬疑", "犯罪"], [8, 8]],
  ["Signal Lost", 2021, 55, ["Sci-Fi", "Drama"], [8]],
  ["深空回响", 2024, 47, ["科幻"], [6]],
  ["青石巷", 2017, 45, ["家庭", "剧情"], [20]],
  ["旧城往事", 2016, 43, ["剧情"], [12, 12]],
  ["极地", 2022, 58, ["冒险"], [6]],
  ["逆流", 2020, 49, ["犯罪", "动作"], [10]],
].map(([title, year, minutes, genres, seasons]) => ({ title, year, minutes, genres, seasons }) as Seed & { seasons: number[] });

const EPISODE_NAMES = ["启程", "暗流", "回声", "失踪", "旧信", "雨夜", "对峙", "真相", "黎明", "归来", "余烬", "终章"];

const VIEWS: EmbyView[] = [
  { id: "v-movies", name: "电影", collectionType: "movies" },
  { id: "v-tv", name: "剧集", collectionType: "tvshows" },
  { id: "v-docs", name: "纪录片", collectionType: "movies" },
  { id: "v-home", name: "家庭视频", collectionType: "homevideos" },
];

const items = new Map<string, EmbyItem>();
const children = new Map<string, EmbyItem[]>();
const pad = (value: number) => String(value).padStart(2, "0");

function makeItem(fields: Partial<EmbyItem> & Pick<EmbyItem, "id" | "name" | "type">): EmbyItem {
  const item: EmbyItem = {
    mediaType: fields.type === "Series" ? null : "Video",
    productionYear: null,
    runTimeTicks: null,
    imageTags: scenario.noArt ? {} : { Primary: "p" },
    backdropImageTags: scenario.noArt || fields.type === "Episode" ? [] : ["b"],
    primaryImageAspectRatio: 2 / 3,
    userData: { playbackPositionTicks: null, played: false, unplayedItemCount: null },
    seriesId: null,
    seriesName: null,
    indexNumber: null,
    parentIndexNumber: null,
    overview: null,
    genres: [],
    people: [],
    studios: [],
    officialRating: null,
    communityRating: null,
    tagline: null,
    premiereDate: null,
    endDate: null,
    status: null,
    mediaStreams: [],
    ...fields,
  };
  items.set(item.id, item);
  return item;
}

function addFlat(viewId: string, prefix: string, type: "Movie" | "Video", seeds: Seed[]) {
  const list = seeds.map((seed, index) => {
    const runTimeTicks = seed.minutes * 60 * TICKS;
    const inProgress = type === "Movie" && index % 7 === 2;
    return makeItem({
      id: prefix + pad(index + 1),
      name: seed.title,
      type,
      productionYear: seed.year,
      runTimeTicks,
      genres: seed.genres,
      overview: type === "Video" ? null : OVERVIEWS[index % OVERVIEWS.length],
      primaryImageAspectRatio: type === "Video" ? 16 / 9 : 2 / 3,
      // One movie without artwork exercises the fallback tile.
      imageTags: scenario.noArt || index === 11 ? {} : { Primary: "p" },
      userData: {
        playbackPositionTicks: inProgress ? Math.round(runTimeTicks * (0.18 + (index % 3) * 0.27)) : null,
        played: index % 5 === 3,
        unplayedItemCount: null,
      },
    });
  });
  children.set(viewId, list);
}

addFlat("v-movies", "m", "Movie", MOVIES);
addFlat("v-docs", "d", "Movie", DOCS);
addFlat("v-home", "h", "Video", HOME_VIDEOS);

const resumeEpisodes: EmbyItem[] = [];
children.set(
  "v-tv",
  SERIES.map((seed, seriesIndex) => {
    const seriesId = "s" + pad(seriesIndex + 1);
    const episodes: EmbyItem[] = [];
    seed.seasons.forEach((count, seasonIndex) => {
      for (let number = 1; number <= count; number += 1) {
        const watched = seriesIndex % 3 === 0 && seasonIndex === 0 && number <= count - 3;
        const runTimeTicks = seed.minutes * 60 * TICKS;
        const resumeHere = seriesIndex % 3 === 0 && seasonIndex === 0 && number === count - 2;
        const episode = makeItem({
          id: seriesId + "e" + (seasonIndex + 1) + pad(number),
          name: EPISODE_NAMES[(number - 1 + seasonIndex * 3) % EPISODE_NAMES.length],
          type: "Episode",
          seriesId,
          seriesName: seed.title,
          indexNumber: number,
          parentIndexNumber: seasonIndex + 1,
          productionYear: seed.year + seasonIndex,
          runTimeTicks,
          primaryImageAspectRatio: 16 / 9,
          overview: OVERVIEWS[(number + seasonIndex) % OVERVIEWS.length],
          userData: {
            playbackPositionTicks: resumeHere ? Math.round(runTimeTicks * 0.42) : null,
            played: watched,
            unplayedItemCount: null,
          },
        });
        episodes.push(episode);
        if (resumeHere) resumeEpisodes.push(episode);
      }
    });
    children.set(seriesId, episodes);
    const unplayed = episodes.filter((episode) => !episode.userData.played).length;
    return makeItem({
      id: seriesId,
      name: seed.title,
      type: "Series",
      productionYear: seed.year,
      genres: seed.genres,
      overview: OVERVIEWS[(seriesIndex + 2) % OVERVIEWS.length],
      userData: { playbackPositionTicks: null, played: unplayed === 0, unplayedItemCount: unplayed },
    });
  }),
);

const RESUME: EmbyItem[] = [
  ...resumeEpisodes,
  ...(children.get("v-movies") ?? []).filter((item) => (item.userData.playbackPositionTicks ?? 0) > 0),
];

const SESSION: EmbySession = {
  serverUrl: "http://emby.home:8096",
  userId: "mock-user",
  userName: "hiccup",
  serverName: "家庭影院",
  deviceId: "mock-device",
};
let session: EmbySession | null = scenario.signedOut ? null : SESSION;

// ---- generated artwork -------------------------------------------------------

function hue(text: string, salt: number): number {
  let hash = 2166136261 ^ salt;
  for (const char of text) hash = Math.imul(hash ^ char.charCodeAt(0), 16777619);
  return Math.abs(hash) % 360;
}

function escapeXml(text: string): string {
  return text.replace(/[&<>"']/g, (char) => "&#" + char.charCodeAt(0) + ";");
}

function svgUrl(svg: string): string {
  return "data:image/svg+xml;charset=utf-8," + encodeURIComponent(svg);
}

const FONT = "Microsoft YaHei UI, PingFang SC, Noto Sans CJK SC, sans-serif";

function posterSvg(id: string, title: string, year: number | null): string {
  const a = hue(id, 1);
  const b = (a + 40 + hue(id, 2) % 80) % 360;
  const width = Array.from(title).reduce((sum, char) => sum + (char.charCodeAt(0) > 0x2e80 ? 1 : 0.56), 0);
  const size = Math.max(22, Math.min(46, 300 / Math.max(width, 1)));
  return svgUrl(
    '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 400 600">' +
      '<defs><linearGradient id="g" x1="0" y1="0" x2="0.4" y2="1">' +
      '<stop offset="0" stop-color="hsl(' + a + ',48%,38%)"/><stop offset="1" stop-color="hsl(' + b + ',55%,9%)"/></linearGradient>' +
      '<radialGradient id="r" cx="0.72" cy="0.22" r="0.55"><stop offset="0" stop-color="hsla(' + a + ',85%,78%,0.6)"/>' +
      '<stop offset="1" stop-color="hsla(' + a + ',85%,78%,0)"/></radialGradient></defs>' +
      '<rect width="400" height="600" fill="url(#g)"/><rect width="400" height="600" fill="url(#r)"/>' +
      '<path d="M0 420 L110 330 L190 392 L290 290 L400 372 L400 600 L0 600 Z" fill="hsla(' + b + ',45%,6%,0.72)"/>' +
      '<text x="200" y="505" text-anchor="middle" font-family="' + FONT + '" font-size="' + size.toFixed(0) + '" font-weight="700" fill="#fff">' + escapeXml(title) + "</text>" +
      (year ? '<text x="200" y="548" text-anchor="middle" font-family="' + FONT + '" font-size="19" letter-spacing="5" fill="rgba(255,255,255,0.68)">' + year + "</text>" : "") +
      "</svg>",
  );
}

function stillSvg(id: string, caption: string | null): string {
  const a = hue(id, 3);
  const b = (a + 150) % 360;
  return svgUrl(
    '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1280 720">' +
      '<defs><linearGradient id="g" x1="0" y1="0" x2="1" y2="1">' +
      '<stop offset="0" stop-color="hsl(' + a + ',42%,30%)"/><stop offset="1" stop-color="hsl(' + b + ',38%,10%)"/></linearGradient>' +
      '<radialGradient id="s" cx="0.3" cy="0.35" r="0.4"><stop offset="0" stop-color="hsla(' + ((a + 30) % 360) + ',90%,80%,0.75)"/>' +
      '<stop offset="1" stop-color="hsla(' + ((a + 30) % 360) + ',90%,80%,0)"/></radialGradient></defs>' +
      '<rect width="1280" height="720" fill="url(#g)"/><rect width="1280" height="720" fill="url(#s)"/>' +
      '<path d="M0 520 C220 430 380 500 560 450 S930 360 1280 460 L1280 720 L0 720 Z" fill="hsla(' + b + ',40%,5%,0.7)"/>' +
      '<path d="M0 600 C300 540 520 610 800 560 S1100 520 1280 570 L1280 720 L0 720 Z" fill="hsla(' + b + ',40%,3%,0.8)"/>' +
      (caption ? '<text x="64" y="660" font-family="' + FONT + '" font-size="40" font-weight="700" fill="rgba(255,255,255,0.85)">' + escapeXml(caption) + "</text>" : "") +
      "</svg>",
  );
}

function portraitSvg(id: string): string {
  const a = hue(id, 7);
  return svgUrl(
    '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 240 240">' +
      '<defs><linearGradient id="g" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="hsl(' + a + ',28%,46%)"/>' +
      '<stop offset="1" stop-color="hsl(' + a + ',32%,20%)"/></linearGradient></defs>' +
      '<rect width="240" height="240" fill="url(#g)"/>' +
      '<circle cx="120" cy="98" r="46" fill="hsla(' + a + ',30%,84%,0.55)"/>' +
      '<path d="M32 240c10-58 46-88 88-88s78 30 88 88z" fill="hsla(' + a + ',30%,84%,0.45)"/>' +
      "</svg>",
  );
}

function imageUrl(req: { itemId?: string; imageType?: string }): string {
  if (req.itemId && PEOPLE.has(req.itemId)) return portraitSvg(req.itemId);
  const item = req.itemId ? items.get(req.itemId) : undefined;
  if (!item) return "";
  if (req.imageType === "Backdrop") return stillSvg(item.id, null);
  if (item.type === "Episode" || item.type === "Video") return stillSvg(item.id, null);
  return posterSvg(item.id, item.name, item.productionYear);
}

// ---- detail data: cast, studios, ratings, streams ------------------------------

const CAST_POOL: Array<[string, boolean]> = [
  ["林一凡", true], ["周素心", true], ["陈默", true], ["许知远", false], ["沈青禾", true],
  ["顾北辰", true], ["苏晚", true], ["韩东", false], ["叶微澜", true], ["秦朗", true],
  ["江离", true], ["白露", false], ["Anna Berg", true], ["Tom Hale", true], ["Mia Chen", false],
  ["王川", true], ["李安然", true], ["赵一鸣", false], ["孟夏", true], ["高远", true],
];
const DIRECTORS = ["张晓舟", "刘念", "Erik Lund", "陈思远", "黄蔚"];
const WRITERS = ["吴桐", "郑楠", "Sara Holm", "宋一"];
const ROLES = ["警探", "林晚", "船长", "记者", "医生", "少年", "母亲", "老板", "科学家", "陌生人", "", ""];
const STUDIOS = [["北辰影业"], ["长河电影", "星火传媒"], ["Nordlys Pictures"], ["青石影视"], ["蓝湾工作室", "北辰影业"]];
const RATINGS = ["PG-13", "R", "PG", "TV-14", "TV-MA", "12+"];
const TAGLINES = ["有些真相，只在雾散之后出现。", "天亮之前，必须做出选择。", "离开得越远，越接近答案。", null, "每个人都有一个回不去的夏天。"];

/** Person id -> name, for the portrait images. */
const PEOPLE = new Map<string, string>();

function person(id: string, name: string, kind: string, role: string | null, photo: boolean): EmbyPerson {
  PEOPLE.set(id, name);
  return { id, name, role, kind, primaryImageTag: photo && !scenario.noArt ? "pt" : null };
}

function streamsFor(seed: number): EmbyMediaStream[] {
  const base: Omit<EmbyMediaStream, "kind"> = {
    index: 0, codec: null, language: null, title: null, displayTitle: null,
    width: null, height: null, channels: null, isDefault: false, isForced: false, isExternal: false,
  };
  const uhd = seed % 4 === 0;
  return [
    { ...base, kind: "Video", index: 0, codec: uhd ? "hevc" : "h264", width: uhd ? 3840 : 1920, height: uhd ? 1606 : 1080, isDefault: true },
    { ...base, kind: "Audio", index: 1, codec: "aac", language: "chi", displayTitle: "国语 AAC 5.1", channels: 6, isDefault: true },
    { ...base, kind: "Audio", index: 2, codec: "ac3", language: "eng", displayTitle: "English AC3 5.1", channels: 6 },
    ...(seed % 3 === 0
      ? []
      : [
          { ...base, kind: "Subtitle", index: 3, codec: "srt", language: "chi", displayTitle: "简体中文 (SRT)", isDefault: true, isExternal: true },
          { ...base, kind: "Subtitle", index: 4, codec: "ass", language: "eng", displayTitle: "English (ASS)" },
        ]),
  ];
}

/** What `emby_item` adds over the list item: cast, studios, ratings, streams. */
function enrich(item: EmbyItem): EmbyItem {
  if (item.type === "Video") return { ...item, mediaStreams: streamsFor(1) };
  const seed = hue(item.id, 9);
  const episode = item.type === "Episode";
  const castCount = episode ? 4 : 9;
  const cast: EmbyPerson[] = [];
  for (let i = 0; i < castCount; i += 1) {
    const index = (seed + i * 7) % CAST_POOL.length;
    const [name, photo] = CAST_POOL[index];
    const kind = episode && i === castCount - 1 ? "GuestStar" : "Actor";
    cast.push(person("pp-" + index, name, kind, ROLES[(seed + i) % ROLES.length] || null, photo));
  }
  const crew = [
    person("pd-" + (seed % DIRECTORS.length), DIRECTORS[seed % DIRECTORS.length], "Director", null, seed % 3 !== 0),
    person("pw-" + (seed % WRITERS.length), WRITERS[seed % WRITERS.length], "Writer", null, false),
  ];
  const series = item.type === "Series";
  const month = String(1 + (seed % 9)).padStart(2, "0");
  return {
    ...item,
    people: [...cast, ...crew],
    studios: STUDIOS[seed % STUDIOS.length],
    officialRating: RATINGS[seed % RATINGS.length],
    communityRating: Math.round((6 + (seed % 35) / 10) * 10) / 10,
    tagline: episode ? null : TAGLINES[seed % TAGLINES.length],
    premiereDate: (item.productionYear ?? 2020) + "-" + month + "-1" + (seed % 9) + "T00:00:00.0000000Z",
    status: series ? (seed % 2 ? "Continuing" : "Ended") : null,
    mediaStreams: series ? [] : streamsFor(seed),
  };
}

// ---- player ------------------------------------------------------------------

const AUDIO: PlayerTrack[] = [
  { id: 1, type: "audio", title: "国语 · AAC 5.1", lang: "chi", selected: true },
  { id: 2, type: "audio", title: "English · AC3 5.1", lang: "eng", selected: false },
  { id: 3, type: "audio", title: "导演评论音轨", lang: "chi", selected: false },
];
const SUBTITLES: PlayerTrack[] = [
  { id: 1, type: "subtitle", title: "简体中文", lang: "chi", selected: true },
  { id: 2, type: "subtitle", title: "繁體中文", lang: "chi", selected: false },
  { id: 3, type: "subtitle", title: "English (SDH)", lang: "eng", selected: false },
];

let snapshot: PlayerSnapshot = {
  seq: 0,
  status: "idle",
  positionSeconds: 0,
  durationSeconds: 0,
  volume: 100,
  muted: false,
  buffering: false,
  videoWidth: 0,
  videoHeight: 0,
  audioTracks: [],
  subtitleTracks: [],
  error: null,
};
let sink: ((next: PlayerSnapshot) => void) | null = null;
let ticker: number | null = null;
let fullscreen = false;

function publish(patch: Partial<PlayerSnapshot>): PlayerSnapshot {
  snapshot = { ...snapshot, ...patch, seq: snapshot.seq + 1 };
  sink?.(snapshot);
  return snapshot;
}

function selectTrack(tracks: PlayerTrack[], id: number): PlayerTrack[] {
  return tracks.map((track) => ({ ...track, selected: track.id === id }));
}

function openPlayer(req: OpenRequest): PlayerSnapshot {
  if (ticker !== null) window.clearInterval(ticker);
  const opened = publish({
    status: "loading",
    positionSeconds: req.startSeconds ?? 0,
    durationSeconds: 0,
    buffering: true,
    audioTracks: [],
    subtitleTracks: [],
    error: null,
  });
  window.setTimeout(() => {
    publish({
      status: "playing",
      buffering: false,
      durationSeconds: 6780,
      videoWidth: 1920,
      videoHeight: 1080,
      audioTracks: AUDIO,
      subtitleTracks: SUBTITLES,
    });
  }, 500);
  ticker = window.setInterval(() => {
    if (snapshot.status === "playing") {
      publish({ positionSeconds: Math.min(snapshot.durationSeconds, snapshot.positionSeconds + 1) });
    }
  }, 1000);
  return opened;
}

// ---- IPC ---------------------------------------------------------------------

const wait = (ms: number) => new Promise((resolve) => window.setTimeout(resolve, ms));
const never = () => new Promise<never>(() => undefined);

function page(list: EmbyItem[], req: Record<string, unknown>): EmbyItemsPage {
  const types = typeof req.includeItemTypes === "string" ? req.includeItemTypes.split(",") : null;
  const filtered = types ? list.filter((item) => types.includes(item.type)) : list;
  const start = typeof req.startIndex === "number" ? req.startIndex : 0;
  const limit = typeof req.limit === "number" ? req.limit : filtered.length;
  return { items: filtered.slice(start, start + limit), totalRecordCount: filtered.length, startIndex: start };
}

async function handle(cmd: string, args: Record<string, unknown>): Promise<unknown> {
  const req = (args.req ?? {}) as Record<string, unknown>;
  switch (cmd) {
    case "emby_session":
      await wait(150);
      return session;
    case "emby_login":
      await wait(600);
      if (req.password === "wrong") throw new Error("401 Unauthorized: invalid username or password");
      session = { ...SESSION, userName: String(req.username || SESSION.userName) };
      return session;
    case "emby_logout":
      await wait(200);
      session = null;
      return null;
    case "emby_views":
      if (scenario.state === "loading") return never();
      await wait(200);
      if (scenario.state === "error") throw new Error("error sending request for url (http://emby.home:8096/Users/mock-user/Views)");
      return VIEWS;
    case "emby_items": {
      if (scenario.state === "loading") return never();
      await wait(250);
      if (scenario.state === "empty") return { items: [], totalRecordCount: 0, startIndex: 0 };
      return page(children.get(String(req.parentId)) ?? [], req);
    }
    case "emby_resume":
      await wait(200);
      if (scenario.noResume || scenario.state === "empty") return { items: [], totalRecordCount: 0, startIndex: 0 };
      return { items: RESUME, totalRecordCount: RESUME.length, startIndex: 0 };
    case "emby_item": {
      await wait(180);
      const item = items.get(String(args.id));
      if (!item) throw new Error("404 Not Found");
      return enrich(item);
    }
    case "emby_similar": {
      await wait(250);
      const item = items.get(String(args.id));
      if (!item) return { items: [], totalRecordCount: 0, startIndex: 0 };
      const pool =
        item.type === "Series"
          ? children.get("v-tv") ?? []
          : [...(children.get("v-movies") ?? []), ...(children.get("v-docs") ?? [])];
      const others = pool.filter((other) => other.id !== item.id);
      const start = hue(item.id, 5) % Math.max(others.length, 1);
      const limit = typeof args.limit === "number" ? args.limit : 12;
      const picked = [...others.slice(start), ...others.slice(0, start)].slice(0, limit);
      return { items: picked, totalRecordCount: picked.length, startIndex: 0 };
    }
    case "emby_image_url":
      return imageUrl(req);
    case "emby_playback_target": {
      await wait(400);
      const item = items.get(String(req.itemId));
      const target: EmbyPlaybackTarget = {
        itemId: String(req.itemId),
        url: "https://media.invalid/Videos/" + String(req.itemId) + "/stream?static=true",
        playSessionId: "mock-play-session",
        mediaSourceId: "mock-source",
        startSeconds: (item?.userData.playbackPositionTicks ?? 0) / TICKS,
      };
      return target;
    }
    case "emby_report_playback":
      return null;

    case "player_subscribe": {
      const channel = args.onEvent as { onmessage?: (next: PlayerSnapshot) => void } | undefined;
      sink = channel?.onmessage ? (next) => channel.onmessage?.(next) : null;
      sink?.(snapshot);
      return null;
    }
    case "player_open":
      return openPlayer(req as unknown as OpenRequest);
    case "player_snapshot":
      return snapshot;
    case "player_set_paused":
      publish({ status: args.paused ? "paused" : "playing" });
      return null;
    case "player_seek":
      publish({ positionSeconds: Math.max(0, Math.min(snapshot.durationSeconds, Number(args.seconds) || 0)) });
      return null;
    case "player_set_volume":
      publish({ volume: Number(args.volume) || 0 });
      return null;
    case "player_set_muted":
      publish({ muted: Boolean(args.muted) });
      return null;
    case "player_set_track":
      if (args.kind === "audio") publish({ audioTracks: selectTrack(snapshot.audioTracks, Number(args.id)) });
      else publish({ subtitleTracks: selectTrack(snapshot.subtitleTracks, Number(args.id)) });
      return null;
    case "player_stop":
      if (ticker !== null) window.clearInterval(ticker);
      ticker = null;
      publish({ status: "idle", positionSeconds: 0, durationSeconds: 0, buffering: false, audioTracks: [], subtitleTracks: [] });
      return null;
    case "player_toggle_fullscreen":
      fullscreen = !fullscreen;
      return fullscreen;
    case "player_ui_mode":
    case "video_sync":
    case "overlay_set_expanded":
    case "overlay_set_interactive":
    case "overlay_drag_window":
      return null;

    case "plugin:window|is_maximized":
    case "plugin:window|is_fullscreen":
      return false;
    default:
      if (cmd.startsWith("plugin:window|")) return null;
      console.warn("[mock] unhandled command", cmd, args);
      return null;
  }
}

export function installMockTauri(): void {
  mockWindows("main");
  mockIPC((cmd, args) => handle(cmd, (args ?? {}) as Record<string, unknown>), {
    shouldMockEvents: true,
  });
  console.info("[mock] Tauri IPC mock installed", scenario);
}
