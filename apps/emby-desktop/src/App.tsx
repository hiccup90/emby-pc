import { useCallback, useEffect, useId, useRef, useState } from "react";
import type { FormEvent, ReactNode } from "react";
import type { ArtKey, DemoMedia, Screen } from "./types";
import { continueWatching, demoMedia, featuredMedia, recentlyAdded } from "./data/demo";
import PlayerPage from "./player/PlayerPage";
import "./styles.css";

if (typeof document !== "undefined" && !document.querySelector('link[rel="icon"]')) {
  const icon = document.createElement("link");
  icon.rel = "icon";
  icon.type = "image/svg+xml";
  const svg = "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 32 32'><rect width='32' height='32' fill='#171a17'/><path d='M16 6 26 16 16 26 6 16Z' fill='none' stroke='#c58b65' stroke-width='2'/></svg>";
  icon.href = `data:image/svg+xml,${encodeURIComponent(svg)}`;
  document.head.append(icon);
}

type IconName = "arrow-left" | "arrow-right" | "play" | "pause" | "volume" | "volume-off" | "captions" | "audio" | "chevron";

function Icon({ name, size = 18 }: { name: IconName; size?: number }) {
  const shared = { fill: "none", stroke: "currentColor", strokeWidth: 1.7, strokeLinecap: "round" as const, strokeLinejoin: "round" as const };
  const paths: Record<IconName, ReactNode> = {
    "arrow-left": <><path d="m11 18-6-6 6-6" /><path d="M5 12h14" /></>,
    "arrow-right": <><path d="M5 12h14" /><path d="m13 6 6 6-6 6" /></>,
    play: <path d="m8 5 11 7-11 7z" fill="currentColor" stroke="none" />,
    pause: <><path d="M8 5h3v14H8z" fill="currentColor" stroke="none" /><path d="M15 5h3v14h-3z" fill="currentColor" stroke="none" /></>,
    volume: <><path d="M4 10v4h4l5 4V6l-5 4H4z" /><path d="M16 9a5 5 0 0 1 0 6" /><path d="M18 6a9 9 0 0 1 0 12" /></>,
    "volume-off": <><path d="M4 10v4h4l5 4V6l-5 4H4z" /><path d="m17 9 5 6m0-6-5 6" /></>,
    captions: <><rect x="3" y="5" width="18" height="14" rx="2" /><path d="M8 10H7a2 2 0 0 0 0 4h1m9-4h-1a2 2 0 0 0 0 4h1" /></>,
    audio: <><path d="M4 10v4h4l5 4V6l-5 4H4z" /><path d="M17 9a5 5 0 0 1 0 6" /></>,
    chevron: <path d="m7 10 5 5 5-5" />,
  };

  return <svg aria-hidden="true" width={size} height={size} viewBox="0 0 24 24" {...shared}>{paths[name]}</svg>;
}

function BrandMark() {
  return <span className="brand-mark" aria-hidden="true"><span /></span>;
}

function artDescription(artKey: ArtKey): string {
  const descriptions: Record<ArtKey, string> = {
    north: "雪山与北方公路",
    orbit: "行星与空间站",
    harbor: "薄雾港口与灯塔",
    forest: "山谷与森林",
    city: "蓝调城市天际线",
    desert: "金色旷野与山脉",
  };
  return descriptions[artKey];
}

function Artwork({ media, className = "" }: { media: DemoMedia; className?: string }) {
  const uniqueId = useId().replace(/:/g, "");
  const skyId = `sky-${media.id}-${uniqueId}`;
  return (
    <svg className={`artwork ${className}`} viewBox="0 0 600 800" role="img" aria-label={artDescription(media.artKey)} preserveAspectRatio="xMidYMid slice">
      <defs>
        <linearGradient id={skyId} x1="0" y1="0" x2="0.85" y2="1">
          <stop offset="0" stopColor="var(--color-moss)" stopOpacity=".62" />
          <stop offset="1" stopColor="var(--color-ink)" />
        </linearGradient>
        <linearGradient id={`shade-${media.id}-${uniqueId}`} x1="0" y1="0" x2="0" y2="1">
          <stop offset="0" stopColor="var(--color-ink)" stopOpacity="0" />
          <stop offset="1" stopColor="var(--color-ink)" stopOpacity=".72" />
        </linearGradient>
      </defs>
      <rect width="600" height="800" fill={`url(#${skyId})`} />
      <circle cx="440" cy="185" r="96" fill="var(--color-copper)" opacity=".78" />
      {media.artKey === "north" && <>
        <path d="M0 450 140 275l86 103 102-160 178 220 94-95v457H0z" fill="var(--color-ink)" opacity=".72" />
        <path d="m95 335 45-60 43 52-31-9-12 22-15-17zM270 276l58-58 61 90-50-21-20 21-19-24z" fill="var(--color-paper)" opacity=".73" />
        <path d="M236 800c42-164 67-219 78-292 10-62 10-114 13-155 30 107 37 157 27 219-13 79-43 141-56 228z" fill="var(--color-copper)" opacity=".72" />
        <path d="M0 508q140-60 270 6t330-8v294H0z" fill="var(--color-moss)" opacity=".52" />
        <path d="M52 520h75m-60 20h114m248-35h116" stroke="var(--color-paper)" strokeOpacity=".32" strokeWidth="3" />
      </>}
      {media.artKey === "orbit" && <>
        <circle cx="290" cy="390" r="190" fill="var(--color-copper)" opacity=".78" />
        <path d="M0 468q145-88 295-27t305-4v363H0z" fill="var(--color-ink)" opacity=".76" />
        <ellipse cx="300" cy="390" rx="258" ry="84" fill="none" stroke="var(--color-paper)" strokeOpacity=".62" strokeWidth="4" transform="rotate(-20 300 390)" />
        <path d="m449 220 43-16-12 42-17-11-17 18z" fill="var(--color-paper)" />
        <circle cx="125" cy="200" r="3" fill="var(--color-paper)" /><circle cx="490" cy="320" r="4" fill="var(--color-paper)" /><circle cx="170" cy="570" r="3" fill="var(--color-paper)" />
      </>}
      {media.artKey === "harbor" && <>
        <path d="M0 395q100-92 205-26 85-134 194-18 86-97 201-2v451H0z" fill="var(--color-ink)" opacity=".66" />
        <path d="M0 490q140-46 275 0t325-3v313H0z" fill="var(--color-moss)" opacity=".45" />
        <path d="M0 560q150-28 300 0t300 0M0 625q150-28 300 0t300 0M0 700q150-28 300 0t300 0" fill="none" stroke="var(--color-paper)" strokeOpacity=".24" strokeWidth="3" />
        <path d="M386 244h72l-8 238h-55z" fill="var(--color-paper)" opacity=".86" />
        <path d="m374 244 48-68 48 68z" fill="var(--color-copper)" />
        <path d="M400 280h42v21h-42z" fill="var(--color-ink)" />
        <path d="M421 305v140" stroke="var(--color-copper)" strokeWidth="4" opacity=".7" />
      </>}
      {media.artKey === "forest" && <>
        <path d="M0 455q130-110 255-15 135-147 345-4v364H0z" fill="var(--color-moss)" opacity=".55" />
        <path d="m-30 578 145-280 145 280zm170 25 175-355 174 355zm202-13 131-260 130 260z" fill="var(--color-ink)" opacity=".76" />
        <path d="M0 664q135-55 275 0t325-12v148H0z" fill="var(--color-copper)" opacity=".48" />
        <path d="M90 490v193m160-268v300m178-253v240m104-197v218" stroke="var(--color-paper)" strokeOpacity=".28" strokeWidth="5" />
      </>}
      {media.artKey === "city" && <>
        <circle cx="420" cy="200" r="68" fill="var(--color-paper)" opacity=".62" />
        <path d="M0 510h78V342h86v126h70V278h108v222h65V364h106v149h87v287H0z" fill="var(--color-ink)" opacity=".8" />
        <path d="M0 650q115-75 257-10t343-15v153H0z" fill="var(--color-moss)" opacity=".52" />
        <path d="M103 378v20m0 25v20m163-130v22m0 26v21m104 41v20m0 28v19m133-84v20m0 26v20" stroke="var(--color-copper)" strokeWidth="8" strokeLinecap="round" />
        <path d="M0 735q130-32 270 0t330-5" fill="none" stroke="var(--color-paper)" strokeOpacity=".32" strokeWidth="3" />
      </>}
      {media.artKey === "desert" && <>
        <path d="M0 490 154 310l112 136 114-207 220 258v303H0z" fill="var(--color-moss)" opacity=".55" />
        <path d="m0 555 170-94 112 37 138-74 180 92v282H0z" fill="var(--color-ink)" opacity=".6" />
        <path d="M0 603q136-102 299-9t301-9v224H0z" fill="var(--color-copper)" opacity=".65" />
        <path d="M0 703q172-69 326 5t274-12v113H0z" fill="var(--color-paper)" opacity=".28" />
        <path d="m268 435 14 16-18-5-17 8 8-18" fill="var(--color-paper)" opacity=".65" />
      </>}
      <rect width="600" height="800" fill={`url(#shade-${media.id}-${uniqueId})`} />
      <path d="M32 34h536v732H32z" fill="none" stroke="var(--color-paper)" strokeOpacity=".13" strokeWidth="2" />
      <text x="54" y="726" fill="var(--color-paper)" opacity=".76" fontSize="14" letterSpacing="4" fontFamily="Georgia, serif">{media.subtitle}</text>
    </svg>
  );
}

function MediaCard({ media, onSelect, progress = false }: { media: DemoMedia; onSelect: (media: DemoMedia) => void; progress?: boolean }) {
  return (
    <article className={`media-card${progress ? " media-card--progress" : ""}`}>
      <button className="media-card__poster-button" type="button" onClick={() => onSelect(media)} aria-label={`查看《${media.title}》详情`}>
        <span className="poster-frame">
          <Artwork media={media} />
          {media.progress > 0 && <span className="poster-progress" aria-hidden="true"><span style={{ width: `${media.progress * 100}%` }} /></span>}
          <span className="poster-open" aria-hidden="true"><Icon name="arrow-right" size={16} /></span>
        </span>
        {progress && <span className="continue-action" aria-hidden="true">继续播放 <Icon name="arrow-right" size={13} /></span>}
      </button>
      <button className="media-card__info" type="button" onClick={() => onSelect(media)} aria-label={`查看《${media.title}》详情${progress ? `，已观看 ${Math.round(media.progress * 100)}%` : ""}`}><span className="media-card__title">{media.title}</span><span className="media-card__meta">{media.kind} <i>·</i> {media.year}</span></button>
    </article>
  );
}

export default function App() {
  const [screen, setScreen] = useState<Screen>("login");
  const [activeMedia, setActiveMedia] = useState<DemoMedia>(featuredMedia);
  const homeHeadingRef = useRef<HTMLHeadingElement>(null);
  const detailHeadingRef = useRef<HTMLHeadingElement>(null);

  const openMedia = useCallback((media: DemoMedia) => {
    setActiveMedia(media);
    setScreen("detail");
  }, []);

  const enterPlayer = useCallback(() => {
    setScreen("player");
  }, []);

  const returnToDetail = useCallback(() => {
    setScreen("detail");
  }, []);

  useEffect(() => {
    if (screen === "home") homeHeadingRef.current?.focus({ preventScroll: true });
    if (screen === "detail") detailHeadingRef.current?.focus({ preventScroll: true });
  }, [screen]);

  useEffect(() => {
    const onKeyDown = (event: KeyboardEvent) => {
      if (event.key === "Escape" && screen === "detail") {
        event.preventDefault();
        setScreen("home");
      }
    };
    window.addEventListener("keydown", onKeyDown);
    return () => window.removeEventListener("keydown", onKeyDown);
  }, [screen]);

  const onServerSubmit = (event: FormEvent<HTMLFormElement>) => {
    event.preventDefault();
    setScreen("home");
  };

  return (
    <div className={`app-shell app-shell--${screen}`}>
      {screen === "login" && <main className="login-layout">
        <section className="login-art" aria-label="演示影片海报">
          <Artwork media={featuredMedia} className="login-art__image" />
          <div className="login-art__veil" />
          <div className="login-art__brand"><BrandMark /><span>EMBER / EMBY</span></div>
          <div className="login-art__caption"><span className="eyebrow">PERSONAL CINEMA · 01</span><p>把今晚，<br />留给一个好故事。</p><span className="login-art__credit">本地视觉演示 · 无远程图片</span></div>
          <span className="login-art__index">A QUIETER WAY TO WATCH <i>—</i> 01 / 06</span>
        </section>
        <section className="login-panel">
          <div className="login-panel__top"><BrandMark /><span>Emby 桌面体验</span></div>
          <div className="login-form-wrap">
            <span className="eyebrow">欢迎回来</span>
            <h1>连接你的<br /><em>媒体世界。</em></h1>
            <p className="login-intro">填写服务器地址只是界面演示；进入后将浏览本地样例影库，不会发起连接。</p>
            <form className="login-form" noValidate onSubmit={onServerSubmit}>
              <label htmlFor="server-address">服务器地址</label>
              <input id="server-address" name="server" type="text" inputMode="url" autoComplete="url" placeholder="例如 emby.local:8096" />
              <button className="button button--primary login-submit" type="submit"><span>进入本地演示影库</span><Icon name="arrow-right" /></button>
            </form>
            <p className="demo-note"><span className="demo-note__dot" />原型演示 · 不会连接服务器或保存地址 · 可留空进入演示影库</p>
          </div>
          <div className="login-footer"><span>EMBY DESKTOP · UI CONCEPT</span><span>Windows 10 / 11</span></div>
        </section>
      </main>}

      {screen === "home" && <>
        <header className="topbar">
          <a className="brand" href="#home" onClick={(event) => { event.preventDefault(); setScreen("home"); }} aria-label="Emby 演示影库首页"><BrandMark /><span>EMBER<span className="brand__sub">EMBY DESKTOP</span></span></a>
          <nav className="main-nav" aria-label="主导航"><button type="button" className="main-nav__item is-active" onClick={() => setScreen("home")}>影库</button><span className="main-nav__divider" /><span className="main-nav__library">全部影片 <Icon name="chevron" size={14} /></span></nav>
          <div className="topbar__right"><span className="server-status"><i />本地演示</span><span className="profile-mark" aria-label="演示用户">演</span></div>
        </header>
        <main className="home-main">
          <section className="feature" aria-labelledby="feature-title">
            <Artwork media={featuredMedia} className="feature__art" />
            <div className="feature__scrim" />
            <div className="feature__content">
              <span className="eyebrow"><span className="eyebrow__rule" />本周精选 <span className="eyebrow__latin">FEATURED FILM</span></span>
              <p className="feature__kicker">{featuredMedia.subtitle}</p>
              <h1 id="feature-title" ref={homeHeadingRef} tabIndex={-1}>{featuredMedia.title}</h1>
              <p className="feature__meta">{featuredMedia.year}<i />{featuredMedia.kind}<i />{featuredMedia.runtime}<i /><span className="rating-star">✳</span> {featuredMedia.rating}</p>
              <p className="feature__summary">{featuredMedia.synopsis}</p>
              <div className="feature__actions"><button className="button button--primary" type="button" onClick={() => openMedia(featuredMedia)}><Icon name="play" size={16} />继续观看</button><button className="button button--quiet" type="button" onClick={() => openMedia(featuredMedia)}>影片详情 <Icon name="arrow-right" size={16} /></button></div>
            </div>
            <div className="feature__rail-mark"><span>01</span><i />06</div>
          </section>
          <section className="media-section" aria-labelledby="continue-title">
            <div className="section-heading"><div><span className="eyebrow">拾起上次的故事</span><h2 id="continue-title">继续观看</h2></div><span className="section-count">{String(continueWatching.length).padStart(2, "0")} <i>/</i> CONTINUE</span></div>
            <div className="media-rail">{continueWatching.map((media) => <MediaCard key={media.id} media={media} onSelect={openMedia} progress />)}</div>
          </section>
          <section className="media-section media-section--recent" aria-labelledby="recent-title">
            <div className="section-heading section-heading--small"><div><span className="eyebrow">刚刚入库</span><h2 id="recent-title">最近添加</h2></div><span className="section-count">LOCAL LIBRARY <i>·</i> {demoMedia.length} 部</span></div>
            <div className="media-rail media-rail--posters">{recentlyAdded.map((media) => <MediaCard key={media.id} media={media} onSelect={openMedia} />)}</div>
          </section>
          <footer className="home-footer"><span><BrandMark />私人影库，静静相伴。</span><span>演示数据 · 尚未连接 Emby</span></footer>
        </main>
      </>}

      {screen === "detail" && <>
        <header className="topbar topbar--detail">
          <button className="icon-button detail-back" type="button" onClick={() => setScreen("home")} aria-label="返回影库"><Icon name="arrow-left" size={19} /></button>
          <a className="brand" href="#home" onClick={(event) => { event.preventDefault(); setScreen("home"); }} aria-label="Emby 演示影库首页"><BrandMark /><span>EMBER<span className="brand__sub">EMBY DESKTOP</span></span></a>
          <div className="topbar__right"><span className="server-status"><i />本地演示</span><span className="profile-mark" aria-label="演示用户">演</span></div>
        </header>
        <main className="detail-main">
          <section className="detail-hero" aria-labelledby="detail-title">
            <Artwork media={activeMedia} className="detail-hero__art" />
            <div className="detail-hero__shade" />
            <div className="detail-hero__content">
              <div className="breadcrumb"><button type="button" onClick={() => setScreen("home")}>影库</button><span>/</span>{activeMedia.kind}</div>
              <p className="detail-subtitle">{activeMedia.subtitle}</p>
              <h1 id="detail-title" ref={detailHeadingRef} tabIndex={-1}>{activeMedia.title}</h1>
              <div className="detail-meta"><span>{activeMedia.year}</span><i /> <span>{activeMedia.kind}</span><i /> <span>{activeMedia.runtime}</span><i /> <span className="rating-star">✳</span> {activeMedia.rating}</div>
              <p className="detail-synopsis">{activeMedia.synopsis}</p>
              <div className="detail-tags">{activeMedia.genres.map((genre) => <span key={genre}>{genre}</span>)}<span className="detail-note">{activeMedia.detailNote}</span></div>
              <div className="detail-actions"><button className="button button--primary" type="button" onClick={enterPlayer}><Icon name="play" size={16} />{activeMedia.progress > 0 ? "继续播放" : "播放"}</button>{activeMedia.progress > 0 && <span className="detail-progress-label">已观看 {Math.round(activeMedia.progress * 100)}%</span>}</div>
            </div>
            <div className="detail-hero__side-note"><span>EMBY LIBRARY / FEATURE {String(demoMedia.findIndex((item) => item.id === activeMedia.id) + 1).padStart(2, "0")}</span><i /></div>
          </section>
          <section className="detail-lower" aria-label="更多影片">
            <div className="detail-lower__intro"><span className="eyebrow">MORE TO DISCOVER</span><h2>影库中的<br />其他故事。</h2><button className="text-button" type="button" onClick={() => setScreen("home")}>返回影库 <Icon name="arrow-right" size={15} /></button></div>
            <div className="media-rail media-rail--detail">{demoMedia.filter((media) => media.id !== activeMedia.id).slice(0, 4).map((media) => <MediaCard key={media.id} media={media} onSelect={openMedia} />)}</div>
          </section>
        </main>
      </>}

      {screen === "player" && <PlayerPage media={activeMedia} onBack={returnToDetail} />}
    </div>
  );
}
