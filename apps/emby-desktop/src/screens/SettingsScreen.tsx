import { useEffect, useState } from "react";
import { getVersion } from "@tauri-apps/api/app";
import { hasTauriRuntime } from "../emby/client";
import { Icon } from "./shared";
import "./SettingsScreen.css";

interface SettingsScreenProps {
  onBack: () => void;
}

type VideoQuality = "default" | "low";

/** All settings are local-only, stored under this prefix. */
const STORAGE_PREFIX = "emby.settings.";

const QUALITY_OPTIONS: Array<{ value: VideoQuality; label: string; title: string }> = [
  { value: "default", label: "默认", title: "原始画质，优先直接播放" },
  { value: "low", label: "低开销", title: "降低解码与渲染开销，适合低配设备" },
];

const PAGE_SIZE_OPTIONS: readonly number[] = [60, 120, 240];
const DEFAULT_PAGE_SIZE = 120;

function readSetting(key: string): string | null {
  try {
    return window.localStorage.getItem(`${STORAGE_PREFIX}${key}`);
  } catch (reason) {
    console.warn("[settings] could not read", key, reason);
    return null;
  }
}

function writeSetting(key: string, value: string): void {
  try {
    window.localStorage.setItem(`${STORAGE_PREFIX}${key}`, value);
  } catch (reason) {
    console.warn("[settings] could not persist", key, reason);
  }
}

function initialQuality(): VideoQuality {
  return readSetting("videoQuality") === "low" ? "low" : "default";
}

function initialAutoPlay(): boolean {
  const stored = readSetting("autoPlay");
  if (stored === "true") return true;
  if (stored === "false") return false;
  return true;
}

function initialPageSize(): number {
  const stored = Number(readSetting("pageSize"));
  return PAGE_SIZE_OPTIONS.includes(stored) ? stored : DEFAULT_PAGE_SIZE;
}

export default function SettingsScreen({ onBack }: SettingsScreenProps) {
  const [quality, setQuality] = useState<VideoQuality>(initialQuality);
  const [autoPlay, setAutoPlay] = useState<boolean>(initialAutoPlay);
  const [pageSize, setPageSize] = useState<number>(initialPageSize);
  const [version, setVersion] = useState("0.1.0");

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

  const chooseQuality = (next: VideoQuality) => {
    setQuality(next);
    writeSetting("videoQuality", next);
  };

  const toggleAutoPlay = () => {
    const next = !autoPlay;
    setAutoPlay(next);
    writeSetting("autoPlay", String(next));
  };

  const changePageSize = (value: string) => {
    const next = Number(value);
    if (!PAGE_SIZE_OPTIONS.includes(next)) return;
    setPageSize(next);
    writeSetting("pageSize", String(next));
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
      </header>

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
                  aria-checked={quality === option.value}
                  title={option.title}
                  className={`settings-segmented__option${
                    quality === option.value ? " settings-segmented__option--active" : ""
                  }`}
                  onClick={() => chooseQuality(option.value)}
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
              aria-checked={autoPlay}
              aria-labelledby="settings-autoplay-label"
              className={`settings-toggle${autoPlay ? " settings-toggle--on" : ""}`}
              onClick={toggleAutoPlay}
            >
              <span className="settings-toggle__thumb" aria-hidden="true" />
              <span className="visually-hidden">{autoPlay ? "已开启" : "已关闭"}</span>
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
              value={pageSize}
              onChange={(event) => changePageSize(event.target.value)}
            >
              {PAGE_SIZE_OPTIONS.map((size) => (
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
    </main>
  );
}
