import { createContext, useContext, useEffect, useRef, useState } from "react";
import type { RefObject } from "react";
import type { EmbyRemoteSearchResult } from "../emby/contract";
import {
  describeEmbyError,
  embyApplyRemoteSearch,
  embyDeleteInfo,
  embyRemoteSearch,
  embyRemoteSearchImage,
} from "../emby/client";
import type { EmbyLibraryItem } from "../emby/useEmby";
import { ErrorNotice, Icon, MediaArtwork } from "./shared";
import "./ItemActions.css";

/** Item types Emby's own Identify dialog accepts. */
export const IDENTIFY_ITEM_TYPES = new Set([
  "Movie",
  "Trailer",
  "Series",
  "Game",
  "BoxSet",
  "Person",
  "Book",
  "MusicAlbum",
  "MusicArtist",
  "MusicVideo",
]);

export interface ItemSelection {
  active: boolean;
  ids: ReadonlySet<string>;
  toggle: (item: EmbyLibraryItem) => void;
}

export interface ItemActionContextValue {
  selection: ItemSelection;
  openMenu: (item: EmbyLibraryItem, x: number, y: number) => void;
}

export const ItemActionContext = createContext<ItemActionContextValue | null>(null);

export function useItemActions(): ItemActionContextValue | null {
  return useContext(ItemActionContext);
}

export interface ItemMenuState {
  item: EmbyLibraryItem;
  x: number;
  y: number;
}

function useModalEscape(onEscape: () => void, enabled = true) {
  useEffect(() => {
    if (!enabled) return undefined;
    const onKeyDown = (event: KeyboardEvent) => {
      if (event.key !== "Escape") return;
      // Capture and stop here so a page-level Escape handler (detail "back")
      // cannot fire underneath an open dialog.
      event.preventDefault();
      event.stopPropagation();
      onEscape();
    };
    document.addEventListener("keydown", onKeyDown, true);
    return () => document.removeEventListener("keydown", onKeyDown, true);
  }, [enabled, onEscape]);
}

/** Return focus to the control that opened a modal, when it is still mounted. */
function useRestoreFocus() {
  const previous = useRef<HTMLElement | null>(
    document.activeElement instanceof HTMLElement ? document.activeElement : null,
  );
  useEffect(
    () => () => {
      if (previous.current?.isConnected) previous.current.focus();
    },
    [],
  );
}

function closeOnOutside(
  ref: RefObject<HTMLElement | null>,
  onClose: () => void,
  enabled = true,
) {
  useEffect(() => {
    if (!enabled) return undefined;
    const onPointerDown = (event: PointerEvent) => {
      if (ref.current && !ref.current.contains(event.target as Node)) onClose();
    };
    const onKeyDown = (event: KeyboardEvent) => {
      if (event.key !== "Escape") return;
      event.preventDefault();
      event.stopPropagation();
      onClose();
    };
    const onScroll = () => onClose();
    const onResize = () => onClose();
    document.addEventListener("pointerdown", onPointerDown);
    document.addEventListener("keydown", onKeyDown, true);
    window.addEventListener("scroll", onScroll, true);
    window.addEventListener("resize", onResize);
    return () => {
      document.removeEventListener("pointerdown", onPointerDown);
      document.removeEventListener("keydown", onKeyDown, true);
      window.removeEventListener("scroll", onScroll, true);
      window.removeEventListener("resize", onResize);
    };
  }, [enabled, onClose, ref]);
}

export function ItemContextMenu({
  menu,
  canIdentify,
  onClose,
  onStartSelection,
  onDelete,
  onIdentify,
}: {
  menu: ItemMenuState;
  canIdentify: boolean;
  onClose: () => void;
  onStartSelection: (item: EmbyLibraryItem) => void;
  onDelete: (item: EmbyLibraryItem) => void;
  onIdentify: (item: EmbyLibraryItem) => void;
}) {
  const ref = useRef<HTMLDivElement>(null);
  closeOnOutside(ref, onClose);

  const width = 188;
  const height = canIdentify ? 148 : 108;
  const left = Math.max(8, Math.min(menu.x, window.innerWidth - width - 8));
  const top = Math.max(8, Math.min(menu.y, window.innerHeight - height - 8));

  return (
    <div
      ref={ref}
      className="item-menu"
      style={{ left, top, width }}
      role="menu"
      aria-label={`${menu.item.name} 操作`}
      onContextMenu={(event) => event.preventDefault()}
    >
      <button
        type="button"
        role="menuitem"
        className="item-menu__item"
        autoFocus
        onClick={() => onStartSelection(menu.item)}
      >
        <Icon name="select" size={16} />
        多选
      </button>
      {canIdentify && (
        <button
          type="button"
          role="menuitem"
          className="item-menu__item"
          onClick={() => onIdentify(menu.item)}
        >
          <Icon name="identify" size={16} />
          识别
        </button>
      )}
      <button
        type="button"
        role="menuitem"
        className="item-menu__item item-menu__item--danger"
        onClick={() => onDelete(menu.item)}
      >
        <Icon name="trash" size={16} />
        删除
      </button>
    </div>
  );
}

export function SelectionBar({
  count,
  canIdentify,
  onDelete,
  onIdentify,
  onCancel,
}: {
  count: number;
  canIdentify: boolean;
  onDelete: () => void;
  onIdentify: () => void;
  onCancel: () => void;
}) {
  return (
    <div className="selection-bar" role="region" aria-label="多选操作">
      <span className="selection-bar__count">已选择 {count} 项</span>
      <button
        type="button"
        className="selection-bar__button"
        disabled={!canIdentify}
        onClick={onIdentify}
      >
        <Icon name="identify" size={15} />
        识别
      </button>
      <button
        type="button"
        className="selection-bar__button selection-bar__button--danger"
        disabled={count === 0}
        onClick={onDelete}
      >
        <Icon name="trash" size={15} />
        删除
      </button>
      <button type="button" className="selection-bar__button" onClick={onCancel}>
        取消
      </button>
    </div>
  );
}

export function DeleteDialog({
  items,
  busy,
  error,
  onCancel,
  onConfirm,
}: {
  items: EmbyLibraryItem[];
  busy: boolean;
  error: string | null;
  onCancel: () => void;
  onConfirm: () => void;
}) {
  const [paths, setPaths] = useState<string[] | null>(null);
  const single = items.length === 1 ? items[0] : null;
  useRestoreFocus();

  useEffect(() => {
    let cancelled = false;
    setPaths(null);
    if (!single) return undefined;
    void embyDeleteInfo(single.id)
      .then((next) => {
        if (!cancelled) setPaths(next);
      })
      .catch(() => {
        // DeleteInfo may require admin; the delete call still reports the real
        // error, so a missing path preview must not block the confirmation.
        if (!cancelled) setPaths([]);
      });
    return () => {
      cancelled = true;
    };
  }, [single]);

  useModalEscape(() => {
    if (!busy) onCancel();
  });

  return (
    <div className="modal-backdrop" role="presentation" onMouseDown={() => !busy && onCancel()}>
      <div
        className="modal item-action-dialog"
        role="dialog"
        aria-modal="true"
        aria-labelledby="delete-dialog-title"
        onMouseDown={(event) => event.stopPropagation()}
      >
        <h2 className="item-action-dialog__title" id="delete-dialog-title">
          删除{items.length > 1 ? ` ${items.length} 项` : `《${single?.name ?? "条目"}》`}
        </h2>
        <p className="item-action-dialog__warning">
          删除会从 Emby 媒体库移除条目；如果服务器允许，还会删除关联的媒体文件，且不可恢复。
        </p>
        {single && paths && paths.length > 0 && (
          <div className="item-action-dialog__paths">
            <p className="item-action-dialog__label">将删除以下文件</p>
            {paths.map((path, index) => (
              <code key={`${path}:${index}`}>{path}</code>
            ))}
          </div>
        )}
        {items.length > 1 && (
          <ul className="item-action-dialog__names">
            {items.slice(0, 8).map((item) => (
              <li key={item.id}>{item.name}</li>
            ))}
            {items.length > 8 && <li>…以及另外 {items.length - 8} 项</li>}
          </ul>
        )}
        {error && <ErrorNotice message={error} />}
        <div className="item-action-dialog__actions">
          <button
            type="button"
            className="button button--quiet"
            autoFocus
            disabled={busy}
            onClick={onCancel}
          >
            取消
          </button>
          <button type="button" className="button button--danger" disabled={busy} onClick={onConfirm}>
            {busy ? "正在删除…" : "删除"}
          </button>
        </div>
      </div>
    </div>
  );
}

export function IdentifyDialog({
  item,
  onClose,
  onApplied,
}: {
  item: EmbyLibraryItem;
  onClose: () => void;
  onApplied: () => void;
}) {
  const [name, setName] = useState(item.name);
  const [year, setYear] = useState(item.productionYear ? String(item.productionYear) : "");
  const [results, setResults] = useState<EmbyRemoteSearchResult[]>([]);
  /** Server proxy URLs, resolved independently so a slow image cannot hold up search. */
  const [images, setImages] = useState<Array<string | null>>([]);
  const [selected, setSelected] = useState(-1);
  const [searching, setSearching] = useState(false);
  const [applying, setApplying] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [replaceImages, setReplaceImages] = useState(true);
  useRestoreFocus();
  const searchedRef = useRef(false);
  const searchGeneration = useRef(0);
  const applyingRef = useRef(false);

  useModalEscape(() => {
    if (!applying) onClose();
  });

  const runSearch = async () => {
    const trimmed = name.trim();
    if (!trimmed) {
      setError("请输入名称后再搜索。");
      return;
    }
    setSearching(true);
    setError(null);
    const generation = ++searchGeneration.current;
    try {
      const found = await embyRemoteSearch({
        itemId: item.id,
        itemType: item.type,
        name: trimmed,
        productionYear: Number(year) || null,
      });
      if (generation !== searchGeneration.current) return;
      // Official Identify stamps the searched item's type onto every provider
      // result before Apply; provider results leave it empty.
      const next = found.map((result) => ({ ...result, Type: item.type }));
      setResults(next);
      setImages(next.map(() => null));
      setSelected(next.length > 0 ? 0 : -1);
      void Promise.all(
        next.map((result) =>
          result.ImageUrl
            ? embyRemoteSearchImage(result.ImageUrl, result.SearchProviderName).catch(() => null)
            : null,
        ),
      ).then((urls) => {
        if (generation === searchGeneration.current) setImages(urls);
      });
    } catch (reason: unknown) {
      if (generation !== searchGeneration.current) return;
      setResults([]);
      setImages([]);
      setSelected(-1);
      setError(describeEmbyError(reason, "识别搜索失败，请稍后重试。"));
    } finally {
      if (generation === searchGeneration.current) setSearching(false);
    }
  };

  useEffect(() => {
    if (searchedRef.current) return;
    searchedRef.current = true;
    void runSearch();
    // The dialog opens once per item; only the initial search is automatic.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [item.id]);

  const apply = async () => {
    const result = results[selected];
    if (!result || applyingRef.current) return;
    applyingRef.current = true;
    setApplying(true);
    setError(null);
    try {
      await embyApplyRemoteSearch({
        itemId: item.id,
        result,
        replaceAllImages: replaceImages,
      });
      onApplied();
    } catch (reason: unknown) {
      applyingRef.current = false;
      setError(describeEmbyError(reason, "应用识别结果失败，请稍后重试。"));
      setApplying(false);
    }
  };

  return (
    <div className="modal-backdrop" role="presentation" onMouseDown={() => !applying && onClose()}>
      <div
        className="modal identify-dialog"
        role="dialog"
        aria-modal="true"
        aria-labelledby="identify-dialog-title"
        onMouseDown={(event) => event.stopPropagation()}
      >
        <div className="identify-dialog__header">
          <h2 className="item-action-dialog__title" id="identify-dialog-title">
            识别「{item.name}」
          </h2>
          <button
            type="button"
            className="item-action-dialog__close"
            aria-label="关闭"
            disabled={applying}
            onClick={onClose}
          >
            ×
          </button>
        </div>
        <form
          className="identify-dialog__search"
          onSubmit={(event) => {
            event.preventDefault();
            void runSearch();
          }}
        >
          <label>
            <span>名称</span>
            <input
              autoFocus
              value={name}
              onChange={(event) => setName(event.target.value)}
              disabled={applying}
            />
          </label>
          <label className="identify-dialog__year">
            <span>年份</span>
            <input
              value={year}
              inputMode="numeric"
              maxLength={4}
              onChange={(event) => setYear(event.target.value.replace(/\D/g, ""))}
              disabled={applying}
            />
          </label>
          <button
            type="submit"
            className="button button--secondary"
            disabled={searching || applying}
          >
            {searching ? "搜索中…" : "搜索"}
          </button>
        </form>

        <div className="identify-dialog__results" aria-live="polite">
          {searching ? (
            <p className="identify-dialog__empty">正在搜索元数据提供方…</p>
          ) : results.length === 0 ? (
            <p className="identify-dialog__empty">没有找到结果，试试修改名称或年份。</p>
          ) : (
            results.map((result, index) => (
              <button
                type="button"
                key={`${result.SearchProviderName ?? "provider"}-${result.Name ?? "result"}-${index}`}
                className={`identify-result${index === selected ? " is-selected" : ""}`}
                aria-pressed={index === selected}
                onClick={() => setSelected(index)}
              >
                <MediaArtwork
                  src={images[index] ?? null}
                  alt=""
                  title={result.Name ?? item.name}
                  fallbackLabel="无图"
                  loading="lazy"
                />
                <span className="identify-result__text">
                  <span className="identify-result__name">{result.Name ?? "未命名"}</span>
                  <span className="identify-result__meta">
                    {[result.ProductionYear, result.SearchProviderName].filter(Boolean).join(" · ")}
                  </span>
                  {result.Overview && (
                    <span className="identify-result__overview">{result.Overview}</span>
                  )}
                </span>
              </button>
            ))
          )}
        </div>

        {error && <ErrorNotice message={error} />}
        <div className="item-action-dialog__actions">
          <label className="identify-dialog__replace">
            <input
              type="checkbox"
              checked={replaceImages}
              onChange={(event) => setReplaceImages(event.target.checked)}
              disabled={applying}
            />
            替换现有图片
          </label>
          <button
            type="button"
            className="button button--quiet"
            disabled={applying}
            onClick={onClose}
          >
            取消
          </button>
          <button
            type="button"
            className="button button--primary"
            disabled={selected < 0 || applying}
            onClick={() => void apply()}
          >
            {applying ? "应用中…" : "应用"}
          </button>
        </div>
      </div>
    </div>
  );
}
