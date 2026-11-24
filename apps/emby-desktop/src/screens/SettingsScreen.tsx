import { useCallback, useEffect, useRef, useState } from "react";
import { getVersion } from "@tauri-apps/api/app";
import type { EmbyView } from "../emby/contract";
import { describeEmbyError, hasTauriRuntime } from "../emby/client";
import { collectionTypeOf, defaultHomeViewIds } from "../emby/library";
import { DEFAULT_APP_SETTINGS, getSettings, saveSettings } from "../settings/tauri";
import type { AppSettings, VideoQuality } from "../settings/tauri";
import type { SkipIntroMode } from "../player/contract";
import { useAsyncResource } from "../emby/useAsyncResource";
import { ErrorNotice, Icon, useEscape } from "./shared";
import "./SettingsScreen.css";

interface SettingsScreenProps {
  /** Emby libraries available for the home page configuration. */
  views: EmbyView[];
  onBack: () => void;
  /** Lets the shell react immediately after a setting is persisted. */
  onSettingsChange?: (settings: AppSettings) => void;
}

function homeViewKind(view: EmbyView): string {
  switch (collectionTypeOf(view)) {
    case "movies":
      return "电影";
    case "tvshows":
      return "电视剧";
    case "homevideos":
      return "家庭视频";
    case "music":
      return "音乐";
    default:
      return "媒体库";
  }
}

const QUALITY_OPTIONS: Array<{ value: VideoQuality; label: string; title: string }> = [
  { value: "default", label: "默认", title: "原始画质，优先直接播放" },
  { value: "low", label: "低开销", title: "降低解码与渲染开销，适合低配设备" },
];

const SKIP_INTRO_OPTIONS: Array<{ value: SkipIntroMode; label: string; title: string }> = [
  { value: "off", label: "关闭", title: "不显示按钮，也不自动跳过" },
  { value: "button", label: "按钮", title: "片头期间显示「跳过片头」按钮" },
  { value: "auto", label: "自动", title: "进入片头后自动跳到片头结束" },
];

const PAGE_SIZE_OPTIONS: readonly number[] = [60, 120, 240];
const SAVED_NOTICE_MS = 2400;

function pageSizeOptionsFor(pageSize: number): readonly number[] {
  const current =
    Number.isFinite(pageSize) && pageSize > 0 ? pageSize : DEFAULT_APP_SETTINGS.pageSize;
  return PAGE_SIZE_OPTIONS.includes(current)
    ? PAGE_SIZE_OPTIONS
    : [...PAGE_SIZE_OPTIONS, current].sort((a, b) => a - b);
}

export default function SettingsScreen({ views, onBack, onSettingsChange }: SettingsScreenProps) {
  const [settings, setSettings] = useState<AppSettings>(() => ({ ...DEFAULT_APP_SETTINGS }));
  const [saving, setSaving] = useState(false);
  const [saveError, setSaveError] = useState<string | null>(null);
  const [saved, setSaved] = useState(false);
  const [version, setVersion] = useState("0.1.0");
  const savedTimerRef = useRef<number | null>(null);
  const onSettingsChangeRef = useRef(onSettingsChange);
  const load = useAsyncResource<AppSettings>(() => getSettings(), "settings", {
    fallbackError: "读取设置失败，已恢复默认值。",
  });

  useEffect(() => {
    onSettingsChangeRef.current = onSettingsChange;
  }, [onSettingsChange]);

  // Mirror the async resource into the editable form state; a failed read
  // falls back to the defaults, matching the previous behaviour.
  useEffect(() => {
    if (load.data) {
      setSettings(load.data);
      setSaved(false);
      onSettingsChangeRef.current?.(load.data);
    } else if (load.status === "error") {
      setSettings({ ...DEFAULT_APP_SETTINGS });
    }
  }, [load.data, load.status]);

  useEffect(() => {
    if (!hasTauriRuntime()) return undefined;
    let cancelled = false;
    void getVersion()
      .then((value) => {
        if (!cancelled && value.trim()) setVersion(value.trim());
      })
      .catch(() => {
        // Keep the bundled fallback version.
      });
    return () => {
      cancelled = true;
    };
  }, []);

  useEscape(onBack);

  useEffect(
    () => () => {
      if (savedTimerRef.current !== null) window.clearTimeout(savedTimerRef.current);
    },
    [],
  );

  const persist = useCallback(async (next: AppSettings) => {
    setSaving(true);
    setSaveError(null);
    setSaved(false);
    try {
      const stored = await saveSettings(next);
      setSettings(stored);
      setSaved(true);
      onSettingsChangeRef.current?.(stored);
      if (savedTimerRef.current !== null) window.clearTimeout(savedTimerRef.current);
      savedTimerRef.current = window.setTimeout(() => {
        setSaved(false);
        savedTimerRef.current = null;
      }, SAVED_NOTICE_MS);
    } catch (reason: unknown) {
      setSaveError(describeEmbyError(reason, "保存设置失败，请稍后重试。"));
    } finally {
      setSaving(false);
    }
  }, []);

  const loading = load.status === "loading";
  const loadError = load.error;
  const busy = loading || saving;
  const pageSizes = pageSizeOptionsFor(settings.pageSize);

  const update = (patch: Partial<AppSettings>) => {
    if (busy) return;
    const next = { ...settings, ...patch };
    setSettings(next);
    void persist(next);
  };

  const selectedHomeIds = settings.homeViewIds ?? defaultHomeViewIds(views);
  const viewById = new Map(views.map((view) => [view.id, view]));
  const selectedHomeViews: EmbyView[] = [];
  const selectedSet = new Set<string>();
  for (const id of selectedHomeIds) {
    const view = viewById.get(id);
    if (view && !selectedSet.has(id)) {
      selectedSet.add(id);
      selectedHomeViews.push(view);
    }
  }
  const unselectedHomeViews = views.filter((view) => !selectedSet.has(view.id));

  const toggleHomeView = (view: EmbyView) => {
    const ordered = selectedHomeViews.map((selected) => selected.id);
    update({
      homeViewIds: selectedSet.has(view.id)
        ? ordered.filter((id) => id !== view.id)
        : [...ordered, view.id],
    });
  };

  const moveHomeView = (index: number, delta: -1 | 1) => {
    const next = selectedHomeViews.map((selected) => selected.id);
    const target = index + delta;
    if (target < 0 || target >= next.length) return;
    [next[index], next[target]] = [next[target], next[index]];
    update({ homeViewIds: next });
  };

  return (
    <main className="page settings-page">
      <header className="settings-header">
        <button type="button" className="settings-header__back" onClick={onBack} aria-label="返回媒体库">
          <Icon name="arrow-left" size={18} />
        </button>
        <div className="settings-header__text">
          <h1 className="settings-header__title">设置</h1>
          <p className="settings-header__subtitle">播放与浏览偏好保存在本机，重启应用后仍然生效。</p>
        </div>
        <span
          className={`settings-header__status${saved ? " settings-header__status--saved" : ""}`}
          role="status"
          aria-live="polite"
        >
          {saving ? (
            <>
              <span className="spinner spinner--small" aria-hidden="true" />
              正在保存…
            </>
          ) : saved ? (
            <>
              <Icon name="check" size={15} />
              已保存
            </>
          ) : null}
        </span>
      </header>

      {loadError && (
        <div className="settings-alert">
          <ErrorNotice
            message={loadError}
            onRetry={load.reload}
            retryLabel="重新读取"
          />
        </div>
      )}
      {saveError && (
        <div className="settings-alert">
          <ErrorNotice
            message={saveError}
            onRetry={() => void persist(settings)}
            retryLabel="重试保存"
          />
        </div>
      )}

      {loading ? (
        <p className="settings-loading" role="status" aria-live="polite">
          <span className="spinner spinner--small" aria-hidden="true" />
          正在读取设置…
        </p>
      ) : (
        <>
          <section className="settings-section" aria-labelledby="settings-home-title">
            <h2 className="settings-section__title" id="settings-home-title">
              首页
            </h2>
            <div className="settings-card">
              <div className="settings-row settings-row--stacked">
                <span className="settings-row__text">
                  <span className="settings-row__label">首页媒体库</span>
                  <span className="settings-row__hint">
                    选择要显示「最近入库」的媒体库，并按顺序排列；每行按窗口宽度显示一行，超出可点「查看全部」。
                  </span>
                </span>
              </div>
              {views.length === 0 ? (
                <div className="settings-row">
                  <span className="settings-row__hint">媒体库读取完成后即可配置。</span>
                </div>
              ) : (
                <>
                  {selectedHomeViews.map((view, index) => (
                    <div className="settings-row settings-home-row" key={view.id}>
                      <span className="settings-row__text">
                        <span className="settings-row__label">{view.name}</span>
                        <span className="settings-row__hint">{homeViewKind(view)} · 已显示</span>
                      </span>
                      <span className="settings-home-row__actions">
                        <button
                          type="button"
                          className="settings-order"
                          title="上移"
                          aria-label={`上移 ${view.name}`}
                          disabled={busy || index === 0}
                          onClick={() => moveHomeView(index, -1)}
                        >
                          <Icon name="chevron-up" size={16} />
                        </button>
                        <button
                          type="button"
                          className="settings-order"
                          title="下移"
                          aria-label={`下移 ${view.name}`}
                          disabled={busy || index === selectedHomeViews.length - 1}
                          onClick={() => moveHomeView(index, 1)}
                        >
                          <Icon name="chevron-down" size={16} />
                        </button>
                        <button
                          type="button"
                          role="switch"
                          aria-checked={true}
                          aria-label={`在首页隐藏 ${view.name}`}
                          disabled={busy}
                          className="settings-toggle settings-toggle--on"
                          onClick={() => toggleHomeView(view)}
                        >
                          <span className="settings-toggle__thumb" aria-hidden="true" />
                          <span className="visually-hidden">已显示</span>
                        </button>
                      </span>
                    </div>
                  ))}
                  {unselectedHomeViews.map((view) => (
                    <div className="settings-row settings-home-row" key={view.id}>
                      <span className="settings-row__text">
                        <span className="settings-row__label">{view.name}</span>
                        <span className="settings-row__hint">{homeViewKind(view)} · 未显示</span>
                      </span>
                      <button
                        type="button"
                        role="switch"
                        aria-checked={false}
                        aria-label={`在首页显示 ${view.name}`}
                        disabled={busy}
                        className="settings-toggle"
                        onClick={() => toggleHomeView(view)}
                      >
                        <span className="settings-toggle__thumb" aria-hidden="true" />
                        <span className="visually-hidden">未显示</span>
                      </button>
                    </div>
                  ))}
                </>
              )}
            </div>
          </section>

          <section className="settings-section" aria-labelledby="settings-playback-title">
            <h2 className="settings-section__title" id="settings-playback-title">
              播放
            </h2>
            <div className="settings-card">
              <div className="settings-row">
                <span className="settings-row__text" id="settings-quality-label">
                  <span className="settings-row__label">播放器画质档</span>
                  <span className="settings-row__hint">
                    默认使用服务端提供的原始画质；低开销会减少解码与渲染占用。
                  </span>
                </span>
                <span className="settings-segmented" role="radiogroup" aria-labelledby="settings-quality-label">
                  {QUALITY_OPTIONS.map((option) => (
                    <button
                      key={option.value}
                      type="button"
                      role="radio"
                      aria-checked={settings.videoQuality === option.value}
                      title={option.title}
                      disabled={busy}
                      className={`settings-segmented__option${
                        settings.videoQuality === option.value ? " settings-segmented__option--active" : ""
                      }`}
                      onClick={() => update({ videoQuality: option.value })}
                    >
                      {option.label}
                    </button>
                  ))}
                </span>
              </div>

              <div className="settings-row">
                <span className="settings-row__text">
                  <span className="settings-row__label" id="settings-autoplay-label">
                    自动连播
                  </span>
                  <span className="settings-row__hint">剧集播放结束后自动播放下一集。</span>
                </span>
                <button
                  type="button"
                  role="switch"
                  aria-checked={settings.autoPlay}
                  aria-labelledby="settings-autoplay-label"
                  disabled={busy}
                  className={`settings-toggle${settings.autoPlay ? " settings-toggle--on" : ""}`}
                  onClick={() => update({ autoPlay: !settings.autoPlay })}
                >
                  <span className="settings-toggle__thumb" aria-hidden="true" />
                  <span className="visually-hidden">{settings.autoPlay ? "已开启" : "已关闭"}</span>
                </button>
              </div>

              <div className="settings-row">
                <span className="settings-row__text" id="settings-skip-intro-label">
                  <span className="settings-row__label">跳过片头</span>
                  <span className="settings-row__hint">
                    需要 Emby 已生成片头标记；无标记的条目不显示也不跳过。
                  </span>
                </span>
                <span
                  className="settings-segmented"
                  role="radiogroup"
                  aria-labelledby="settings-skip-intro-label"
                >
                  {SKIP_INTRO_OPTIONS.map((option) => (
                    <button
                      key={option.value}
                      type="button"
                      role="radio"
                      aria-checked={settings.skipIntro === option.value}
                      title={option.title}
                      disabled={busy}
                      className={`settings-segmented__option${
                        settings.skipIntro === option.value ? " settings-segmented__option--active" : ""
                      }`}
                      onClick={() => update({ skipIntro: option.value })}
                    >
                      {option.label}
                    </button>
                  ))}
                </span>
              </div>

              <div className="settings-row">
                <label className="settings-row__text" htmlFor="settings-page-size">
                  <span className="settings-row__label">每页条目数</span>
                  <span className="settings-row__hint">媒体库每次加载的条目数量。</span>
                </label>
                <select
                  id="settings-page-size"
                  className="settings-select"
                  value={settings.pageSize}
                  disabled={busy}
                  onChange={(event) => update({ pageSize: Number(event.target.value) })}
                >
                  {pageSizes.map((size) => (
                    <option key={size} value={size}>
                      {size} 项
                    </option>
                  ))}
                </select>
              </div>
            </div>
          </section>

          <section className="settings-section" aria-labelledby="settings-about-title">
            <h2 className="settings-section__title" id="settings-about-title">
              关于
            </h2>
            <div className="settings-card">
              <div className="settings-row">
                <span className="settings-row__text">
                  <span className="settings-row__label">应用版本</span>
                  <span className="settings-row__hint">Emby Desktop</span>
                </span>
                <span className="settings-value">v{version}</span>
              </div>
              <div className="settings-row settings-row--stacked">
                <span className="settings-row__text">
                  <span className="settings-row__label">mpv 配置文件</span>
                  <span className="settings-row__hint">
                    播放器高级参数保存在此文件中，修改后重启播放器生效。
                  </span>
                </span>
                <code className="settings-path">%APPDATA%\com.ember.emby-desktop\mpv.conf</code>
              </div>
            </div>
          </section>
        </>
      )}
    </main>
  );
}
