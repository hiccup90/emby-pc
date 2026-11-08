import { useState } from "react";
import type { FormEvent } from "react";
import type { EmbyCredentials } from "../emby/client";
import { BrandMark, Icon } from "./shared";

interface LoginScreenProps {
  desktop: boolean;
  pending: boolean;
  loginError: string | null;
  startupError: string | null;
  onLogin: (credentials: EmbyCredentials) => Promise<boolean>;
  onPreview: () => void;
  onClearError: () => void;
}

function normalizeServer(raw: string): string {
  const value = raw.trim();
  if (!value) return "";
  if (/^https?:\/\//i.test(value)) return value;
  return `http://${value}`;
}

export default function LoginScreen({
  desktop,
  pending,
  loginError,
  startupError,
  onLogin,
  onPreview,
  onClearError,
}: LoginScreenProps) {
  const [server, setServer] = useState("");
  const [username, setUsername] = useState("");
  const [password, setPassword] = useState("");
  const [validationError, setValidationError] = useState<string | null>(null);

  const clearErrors = () => {
    setValidationError(null);
    onClearError();
  };

  const submit = async (event: FormEvent<HTMLFormElement>) => {
    event.preventDefault();
    if (!desktop || pending) return;

    const normalizedServer = normalizeServer(server);
    const cleanUsername = username.trim();
    if (!normalizedServer || !cleanUsername || !password) {
      setValidationError("请填写服务器地址、用户名和密码。");
      return;
    }

    try {
      new URL(normalizedServer);
    } catch {
      setValidationError("服务器地址格式无效，请填写完整地址或主机名。");
      return;
    }

    setValidationError(null);
    await onLogin({
      server: normalizedServer,
      username: cleanUsername,
      password,
    });
  };

  const visibleError = validationError ?? loginError;

  return (
    <main className="login-layout">
      <section className="login-art" aria-label="Emby 私人影库视觉">
        <div className="login-art__canvas" aria-hidden="true">
          <span className="login-art__disc login-art__disc--one" />
          <span className="login-art__disc login-art__disc--two" />
          <span className="login-art__grid" />
          <span className="login-art__word">EMBER</span>
        </div>
        <div className="login-art__veil" />
        <div className="login-art__brand">
          <BrandMark />
          <span>EMBER / EMBY</span>
        </div>
        <div className="login-art__caption">
          <span className="eyebrow">PERSONAL CINEMA · 01</span>
          <p>
            把今晚，
            <br />
            留给一个好故事。
          </p>
          <span className="login-art__credit">YOUR SERVER · YOUR LIBRARY</span>
        </div>
        <span className="login-art__index">
          A QUIETER WAY TO WATCH <i>—</i> EMBY
        </span>
      </section>

      <section className="login-panel">
        <div className="login-panel__top">
          <BrandMark />
          <span>Emby 桌面体验</span>
        </div>
        <div className="login-form-wrap">
          <span className="eyebrow">欢迎回来</span>
          <h1>
            连接你的
            <br />
            <em>媒体世界。</em>
          </h1>
          <p className="login-intro">
            输入 Emby 服务器与账户，浏览真实影库、查看详情，并把播放交给本机 mpv。
          </p>

          {!desktop && (
            <div className="preview-callout" role="status">
              <span className="preview-callout__tag">PREVIEW MODE</span>
              <span>预览模式：需要 Tauri 桌面环境连接 Emby。</span>
            </div>
          )}

          {startupError && (
            <div className="form-alert" role="alert">
              {startupError}
            </div>
          )}

          <form
            className="login-form"
            noValidate
            onSubmit={(event) => void submit(event)}
            aria-busy={pending}
          >
            <label htmlFor="server-address">服务器地址</label>
            <input
              id="server-address"
              name="server"
              type="text"
              inputMode="url"
              autoComplete="url"
              spellCheck={false}
              placeholder="例如 https://emby.local:8920"
              value={server}
              disabled={pending}
              onChange={(event) => {
                setServer(event.target.value);
                clearErrors();
              }}
            />

            <label htmlFor="emby-username">用户名</label>
            <input
              id="emby-username"
              name="username"
              type="text"
              autoComplete="username"
              spellCheck={false}
              placeholder="Emby 用户名"
              value={username}
              disabled={pending}
              onChange={(event) => {
                setUsername(event.target.value);
                clearErrors();
              }}
            />

            <label htmlFor="emby-password">密码</label>
            <input
              id="emby-password"
              name="password"
              type="password"
              autoComplete="current-password"
              placeholder="Emby 密码"
              value={password}
              disabled={pending}
              onChange={(event) => {
                setPassword(event.target.value);
                clearErrors();
              }}
            />

            {visibleError && (
              <div className="form-alert" role="alert">
                {visibleError}
              </div>
            )}

            <button
              className="button button--primary login-submit"
              type="submit"
              disabled={!desktop || pending}
            >
              <span>{pending ? "正在连接…" : "连接 Emby"}</span>
              <Icon name="arrow-right" />
            </button>
          </form>

          {!desktop && (
            <button
              className="button button--quiet login-preview-button"
              type="button"
              onClick={onPreview}
            >
              仅预览界面
              <Icon name="arrow-right" size={15} />
            </button>
          )}

          <p className="demo-note">
            <span className="demo-note__dot" />
            {desktop
              ? "凭据由桌面客户端发送至本机 Emby 连接层，不在页面中保存。"
              : "当前为浏览器预览，不会发起任何 Emby 请求。"}
          </p>
        </div>
        <div className="login-footer">
          <span>EMBY DESKTOP · REAL LIBRARY</span>
          <span>Windows 10 / 11</span>
        </div>
      </section>
    </main>
  );
}
