import { useState } from "react";
import type { FormEvent } from "react";
import type { EmbyCredentials } from "../emby/client";

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
    <main className="login-page">
      <section className="login-panel" aria-labelledby="login-title">
        <h1 id="login-title" className="login-title">
          登录 Emby
        </h1>
        <p className="login-subtitle">
          连接你的 Emby 服务器，浏览媒体库并使用本机 mpv 播放。
        </p>

        {!desktop && (
          <div className="preview-callout" role="status">
            预览模式：浏览器中不会连接 Emby。请在 Tauri 桌面客户端中登录。
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
          <div className="field">
            <label htmlFor="server-address">服务器地址</label>
            <input
              id="server-address"
              name="server"
              type="text"
              inputMode="url"
              autoComplete="url"
              spellCheck={false}
              placeholder="https://emby.local:8920"
              value={server}
              disabled={pending}
              onChange={(event) => {
                setServer(event.target.value);
                clearErrors();
              }}
            />
          </div>

          <div className="field">
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
          </div>

          <div className="field">
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
          </div>

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
            {pending ? "正在连接…" : "连接 Emby"}
          </button>
        </form>

        {!desktop && (
          <button
            className="button button--secondary login-secondary"
            type="button"
            onClick={onPreview}
          >
            仅预览界面
          </button>
        )}

      </section>
    </main>
  );
}
