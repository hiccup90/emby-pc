import { useEffect, useState } from "react";
import type { FormEvent } from "react";
import type { EmbyCredentials } from "../emby/client";
import type { EmbySessionReason } from "../emby/contract";
import { BrandMark } from "./shared";

interface LoginScreenProps {
  desktop: boolean;
  pending: boolean;
  loginError: string | null;
  /** Why the boot ended on the login page; "none" is a normal login. */
  reason: EmbySessionReason | null;
  lastServer: string | null;
  lastUserName: string | null;
  onRetry: () => void;
  onLogin: (credentials: EmbyCredentials) => Promise<boolean>;
  onPreview: () => void;
  onClearError: () => void;
}

interface ReasonNotice {
  text: string;
  retryable: boolean;
}

function reasonNotice(reason: EmbySessionReason | null): ReasonNotice | null {
  switch (reason) {
    case "token_rejected":
      return { text: "登录已过期，请重新登录。", retryable: false };
    case "unreachable":
    case "server_error":
      return { text: "无法连接服务器。", retryable: true };
    case "storage_error":
      return { text: "无法读取本地登录信息。", retryable: true };
    default:
      return null;
  }
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
  reason,
  lastServer,
  lastUserName,
  onRetry,
  onLogin,
  onPreview,
  onClearError,
}: LoginScreenProps) {
  const [server, setServer] = useState(lastServer ?? "");
  const [username, setUsername] = useState(lastUserName ?? "");
  const [password, setPassword] = useState("");
  const [validationError, setValidationError] = useState<string | null>(null);

  useEffect(() => {
    if (lastServer) setServer(lastServer);
  }, [lastServer]);

  useEffect(() => {
    if (lastUserName) setUsername(lastUserName);
  }, [lastUserName]);

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
  const notice = reasonNotice(reason);

  return (
    <main className="login-page">
      <section className="login-card" aria-labelledby="login-title">
        <BrandMark large />
        <h1 id="login-title" className="login-title">
          登录 Emby
        </h1>
        <p className="login-subtitle">连接你的 Emby 服务器，浏览媒体库并使用本机 mpv 播放。</p>

        {!desktop && (
          <div className="notice" role="status">
            预览模式：浏览器中不会连接 Emby。请在 Tauri 桌面客户端中登录。
          </div>
        )}

        {notice && (
          <div className="form-alert" role="alert">
            {notice.text}
          </div>
        )}

        {notice?.retryable && (
          <button
            className="button button--secondary button--block"
            type="button"
            onClick={onRetry}
          >
            重试
          </button>
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
            className="button button--primary button--large button--block"
            type="submit"
            disabled={!desktop || pending}
          >
            {pending && <span className="spinner spinner--small spinner--dark" aria-hidden="true" />}
            {pending ? "正在连接…" : "登录"}
          </button>
        </form>

        {!desktop && (
          <button
            className="button button--ghost button--block"
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
