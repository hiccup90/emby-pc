import { useCallback, useEffect, useRef, useState } from "react";
import { describeEmbyError } from "./client";

export type AsyncStatus = "idle" | "loading" | "ready" | "error";

export interface AsyncResource<T> {
  status: AsyncStatus;
  data: T | null;
  error: string | null;
  /** True while a request is in flight. */
  pending: boolean;
  reload: () => void;
}

interface AsyncResourceOptions {
  /** When false the hook stays idle and clears its data. Defaults to true. */
  enabled?: boolean;
  /** Fallback text used when the error has no message. */
  fallbackError?: string;
}

/**
 * One shared async resource state machine: loading/error/ready + retry and
 * cancellation on key change or unmount. `key` is a plain string so callers do
 * not need to memoize a dependency array; changing it starts a new request.
 */
export function useAsyncResource<T>(
  load: (isCancelled: () => boolean) => Promise<T>,
  key: string,
  options: AsyncResourceOptions = {},
): AsyncResource<T> {
  const { enabled = true, fallbackError = "加载失败，请稍后重试。" } = options;
  const [status, setStatus] = useState<AsyncStatus>(enabled ? "loading" : "idle");
  const [data, setData] = useState<T | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [reloadToken, setReloadToken] = useState(0);
  const loadRef = useRef(load);
  loadRef.current = load;

  useEffect(() => {
    if (!enabled) {
      setStatus("idle");
      setData(null);
      setError(null);
      return undefined;
    }

    let cancelled = false;
    setStatus("loading");
    setError(null);
    void loadRef.current(() => cancelled)
      .then((next) => {
        if (cancelled) return;
        setData(next);
        setStatus("ready");
      })
      .catch((reason: unknown) => {
        if (cancelled) return;
        setData(null);
        setError(describeEmbyError(reason, fallbackError));
        setStatus("error");
      });
    return () => {
      cancelled = true;
    };
  }, [enabled, fallbackError, key, reloadToken]);

  const reload = useCallback(() => setReloadToken((value) => value + 1), []);

  return { status, data, error, pending: status === "loading", reload };
}