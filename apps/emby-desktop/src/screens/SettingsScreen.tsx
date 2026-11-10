import { useCallback, useEffect, useRef, useState } from "react";
import { getVersion } from "@tauri-apps/api/app";
import { describeEmbyError, hasTauriRuntime } from "../emby/client";
import { DEFAULT_APP_SETTINGS, getSettings, saveSettings } from "../settings/tauri";
import type { AppSettings, VideoQuality } from "../settings/tauri";
import { ErrorNotice, Icon } from "./shared";
import "./SettingsScreen.css";

interface SettingsScreenProps {
  onBack: () => void;
  /** Lets the shell react immediately after a setting is persisted. */
  onSettingsChange?: (settings: AppSettings) => void;
}

const QUALITY_OPTIONS: Array<{ value: VideoQuality; label: string; title: string }> = [
  { value: "default", label: "默认", title: "原始画质，优先直接播放" },
  { value: "low", label: "低开销", title: "降低解码与渲染开销，适合低配设备" },
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

export default function SettingsScreen({ onBack, onSettingsChange }: SettingsScreenProps) {
  const [settings, setSettings] = useState<AppSettings>(() => ({ ...DEFAULT_APP_SETTINGS }));
  const [loading, setLoading] = useState(true);
  const [loadError, setLoadError] = useState<string | null>(null);
  const [saving, setSaving] = useState(false);
  const [saveError, setSaveError] = useState<string | null>(null);
  const [saved, setSaved] = useState(false);
  const [version, setVersion] = useState("0.1.0");
  const [reloadToken, setReloadToken] = useState(0);
  const savedTimerRef = useRef<number | null>(null);
  const onSettingsChangeRef = useRef(onSettingsChange);

  useEffect(() => {
    onSettingsChangeRef.current = onSettingsChange;
  }, [onSettingsChange]);

  // Read the persisted settings once; a failure falls back to the defaults.
  useEffect(() => {
    let cancelled = false;
    setLoading(true);
    setLoadError(null);
    setSaved(false);
    void getSettings()
      .then((stored) => {
        if (cancelled) return;
        setSettings(stored);
        setLoading(false);
        onSettingsChangeRef.current?.(stored);
      })
      .catch((reason: unknown) => {
        if (cancelled) return;
        setSettings({ ...DEFAULT_APP_SETTINGS });
        setLoadError(describeEmbyError(reason, "读取设置失败，已恢复默认值。"));
        setLoading(false);
      });
    return () => {
      cancelled = true;
    };
  }, [reloadToken]);

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

  useEffect(() => {
    const onKeyDown = (event: KeyboardEvent) => {
      if (event.key === "Escape" && !event.defaultPrevented) {
        event.preventDefault();
        onBack();
      }
    };
    window.addEventListener("keydown", onKeyDown);
    return () => window.removeEventListener("keydown", onKeyDown);
  }, [onBack]);

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
      setLoadError(null);
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

  const update = (patch: Partial<AppSettings>) => {
    if (loading || saving) return;
    const next = { ...settings, ...patch };
    setSettings(next);
    void persist(next);
  };

  const busy = loading || saving;
  const pageSizes = pageSizeOptionsFor(settings.pageSize);

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
            onRetry={() => setReloadToken((value) => value + 1)}
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
