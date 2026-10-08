/**
 * Gmail keyboard shortcuts [WP-E].
 *
 * One global keydown listener (`installShortcutListener`, mounted by AppShell — and on each
 * same-origin email iframe by EmailFrame, so shortcuts keep working after a click into a
 * message body) normalizes the key, applies the guards (typing in inputs / editors, IME composition, modifier chords, open
 * dialogs / menus, the compose dock) and dispatches an action to the most recently registered
 * layer that handles it (`registerShortcuts` / `useShortcuts`): AppShell registers the global
 * ones (c, /, ?, g-sequences), the thread list and the thread view register theirs on top.
 *
 *   c 写邮件 · / 搜索 · j/k 下一个/上一个 · o / Enter 打开 · u 返回列表 · e 归档 · # 删除 ·
 *   ! 举报垃圾邮件 · s 星标 · x 选择 · Shift+I / Shift+U 标为已读/未读 · r 回复 · a 回复全部 ·
 *   f 转发 · ? 快捷键帮助 · g i/s/t/d/a 转到收件箱/已加星标/已发送/草稿/所有邮件
 */
import { useEffect, useRef } from 'react';
import type { MessageKey } from '@/i18n/zh';

export type ShortcutAction =
  | 'compose'
  | 'search'
  | 'next'
  | 'prev'
  | 'open'
  | 'back'
  | 'archive'
  | 'delete'
  | 'spam'
  | 'star'
  | 'select'
  | 'markRead'
  | 'markUnread'
  | 'reply'
  | 'replyAll'
  | 'forward'
  | 'help'
  | 'goInbox'
  | 'goStarred'
  | 'goSent'
  | 'goDrafts'
  | 'goAll';

export type ShortcutHandlers = Partial<Record<ShortcutAction, () => void>>;

/** Normalized key → action. Letters are lower-case, Shift+letter upper-case. */
export const KEY_ACTIONS: Readonly<Record<string, ShortcutAction>> = {
  c: 'compose',
  '/': 'search',
  j: 'next',
  k: 'prev',
  o: 'open',
  Enter: 'open',
  u: 'back',
  e: 'archive',
  '#': 'delete',
  '!': 'spam',
  s: 'star',
  x: 'select',
  I: 'markRead',
  U: 'markUnread',
  r: 'reply',
  a: 'replyAll',
  f: 'forward',
  '?': 'help',
};

/** Second key after `g`. */
export const G_SEQUENCES: Readonly<Record<string, ShortcutAction>> = {
  i: 'goInbox',
  s: 'goStarred',
  t: 'goSent',
  d: 'goDrafts',
  a: 'goAll',
};

/** How long `g` waits for its second key (ms). */
export const SEQUENCE_TIMEOUT_MS = 1500;

/** How the keys of a help row combine: one after another, held together, or alternatives. */
export type ShortcutJoin = 'then' | 'plus' | 'or';

/** Rows of the 键盘快捷键 help dialog. */
export const SHORTCUT_HELP: readonly {
  group: 'actions' | 'navigation' | 'thread';
  keys: string[];
  label: MessageKey;
  join?: ShortcutJoin;
}[] = [
    { group: 'navigation', keys: ['c'], label: 'mail.shortcuts.items.compose' },
    { group: 'navigation', keys: ['/'], label: 'mail.shortcuts.items.search' },
    { group: 'navigation', keys: ['j'], label: 'mail.shortcuts.items.next' },
    { group: 'navigation', keys: ['k'], label: 'mail.shortcuts.items.prev' },
    { group: 'navigation', keys: ['o', 'Enter'], label: 'mail.shortcuts.items.open', join: 'or' },
    { group: 'navigation', keys: ['u'], label: 'mail.shortcuts.items.back' },
    { group: 'navigation', keys: ['g', 'i'], label: 'mail.shortcuts.items.goInbox', join: 'then' },
    { group: 'navigation', keys: ['g', 's'], label: 'mail.shortcuts.items.goStarred', join: 'then' },
    { group: 'navigation', keys: ['g', 't'], label: 'mail.shortcuts.items.goSent', join: 'then' },
    { group: 'navigation', keys: ['g', 'd'], label: 'mail.shortcuts.items.goDrafts', join: 'then' },
    { group: 'navigation', keys: ['g', 'a'], label: 'mail.shortcuts.items.goAll', join: 'then' },
    { group: 'actions', keys: ['e'], label: 'mail.shortcuts.items.archive' },
    { group: 'actions', keys: ['#'], label: 'mail.shortcuts.items.delete' },
    { group: 'actions', keys: ['!'], label: 'mail.shortcuts.items.spam' },
    { group: 'actions', keys: ['s'], label: 'mail.shortcuts.items.star' },
    { group: 'actions', keys: ['x'], label: 'mail.shortcuts.items.select' },
    { group: 'actions', keys: ['Shift', 'I'], label: 'mail.shortcuts.items.markRead', join: 'plus' },
    { group: 'actions', keys: ['Shift', 'U'], label: 'mail.shortcuts.items.markUnread', join: 'plus' },
    { group: 'thread', keys: ['r'], label: 'mail.shortcuts.items.reply' },
    { group: 'thread', keys: ['a'], label: 'mail.shortcuts.items.replyAll' },
    { group: 'thread', keys: ['f'], label: 'mail.shortcuts.items.forward' },
    { group: 'thread', keys: ['?'], label: 'mail.shortcuts.items.help' },
];

// ───────────── guards ─────────────

const NON_TEXT_INPUTS = new Set(['checkbox', 'radio', 'button', 'submit', 'reset', 'range', 'color', 'file', 'image']);

/**
 * Element check that also works across realms: key events from the same-origin email iframe
 * (EmailFrame installs this listener on the frame window) carry targets from the frame's
 * document, which are not `instanceof` the parent window's `Element`.
 */
function asElement(target: EventTarget | null): Element | null {
  if (!target || typeof target !== 'object') return null;
  const node = target as Partial<Element> & { nodeType?: number };
  return node.nodeType === 1 && typeof node.closest === 'function' ? (target as Element) : null;
}

/** Typing targets: text inputs, textareas, selects and anything contenteditable. */
export function isEditableTarget(target: EventTarget | null): boolean {
  const el = asElement(target);
  if (!el) return false;
  const tag = el.tagName;
  if (tag === 'TEXTAREA' || tag === 'SELECT') return true;
  if (tag === 'INPUT') return !NON_TEXT_INPUTS.has(((el as HTMLInputElement).type || 'text').toLowerCase());
  if ((el as HTMLElement).isContentEditable) return true;
  return el.closest('[contenteditable]:not([contenteditable="false"])') !== null;
}

/** Shortcuts never fire inside these (dialogs, menus, popovers, the compose dock). */
const BLOCKING_CONTAINERS =
  '[role="dialog"],[role="alertdialog"],[role="menu"],[role="listbox"],[data-no-shortcuts],.compose-dock';

/** Focusable controls whose own Enter / Space behaviour must win. */
const INTERACTIVE = 'button,a[href],[role="button"],[role="checkbox"],[role="menuitem"],[role="tab"],[role="switch"],summary';

/**
 * A keydown that belongs to an IME composition (Chinese / Japanese input methods): the Enter
 * that confirms the candidate, the Esc that cancels it, Backspace inside the composition…
 * Chrome reports `isComposing`; Safari sends keyCode 229 (also right after compositionend).
 * Accepts DOM and React keyboard events.
 */
export function isImeKeyEvent(e: { isComposing?: boolean; keyCode?: number; nativeEvent?: { isComposing?: boolean; keyCode?: number } }): boolean {
  return !!(e.isComposing || e.nativeEvent?.isComposing || e.keyCode === 229 || e.nativeEvent?.keyCode === 229);
}

/** Whether a keydown must be left alone. */
export function shouldIgnoreKeyEvent(e: KeyboardEvent): boolean {
  if (e.defaultPrevented) return true;
  // IME composition (Chinese input methods): keyCode 229 while composing.
  if (isImeKeyEvent(e)) return true;
  if (e.ctrlKey || e.metaKey || e.altKey) return true;
  const target = e.target;
  if (isEditableTarget(target)) return true;
  const el = asElement(target);
  if (el) {
    if (el.closest(BLOCKING_CONTAINERS)) return true;
    if ((e.key === 'Enter' || e.key === ' ') && el.closest(INTERACTIVE)) return true;
  }
  return false;
}

/**
 * Normalized key: single letters are lower-case, or upper-case with Shift (CapsLock is
 * ignored); other printable characters as typed ("#", "!", "?", "/"); "Enter". Others → null.
 */
export function normalizeKey(e: Pick<KeyboardEvent, 'key' | 'shiftKey'>): string | null {
  const k = e.key;
  if (k === 'Enter') return k;
  if (k.length !== 1) return null;
  if (/[a-z]/i.test(k)) return e.shiftKey ? k.toUpperCase() : k.toLowerCase();
  return k;
}

// ───────────── registry ─────────────

interface Layer {
  id: number;
  get: () => ShortcutHandlers;
}

const layers: Layer[] = [];
let layerSeq = 0;

/** Adds a handler layer (latest wins per action). Returns the unregister function. */
export function registerShortcuts(get: () => ShortcutHandlers): () => void {
  const layer = { id: ++layerSeq, get };
  layers.push(layer);
  return () => {
    const i = layers.findIndex((l) => l.id === layer.id);
    if (i >= 0) layers.splice(i, 1);
  };
}

/** Runs `action` on the top-most layer that handles it; false when none does. */
export function dispatchShortcut(action: ShortcutAction): boolean {
  for (let i = layers.length - 1; i >= 0; i--) {
    const fn = layers[i]?.get()[action];
    if (fn) {
      fn();
      return true;
    }
  }
  return false;
}

export interface KeydownHandlerOptions {
  now?: () => number;
  sequenceTimeoutMs?: number;
  dispatch?: (action: ShortcutAction) => boolean;
}

/** Builds the keydown listener (exported for tests; the app uses installShortcutListener). */
export function createKeydownHandler(opts: KeydownHandlerOptions = {}): (e: KeyboardEvent) => void {
  const now = opts.now ?? (() => Date.now());
  const timeout = opts.sequenceTimeoutMs ?? SEQUENCE_TIMEOUT_MS;
  const dispatch = opts.dispatch ?? dispatchShortcut;
  let gPressedAt: number | null = null;

  return (e: KeyboardEvent) => {
    if (shouldIgnoreKeyEvent(e)) {
      gPressedAt = null;
      return;
    }
    const key = normalizeKey(e);
    if (key === null) return;
    if (gPressedAt !== null) {
      const fresh = now() - gPressedAt <= timeout;
      gPressedAt = null;
      if (fresh) {
        const seq = G_SEQUENCES[key];
        if (seq && dispatch(seq)) e.preventDefault();
        return;
      }
    }
    if (key === 'g') {
      gPressedAt = now();
      return;
    }
    const action = KEY_ACTIONS[key];
    if (action && dispatch(action)) e.preventDefault();
  };
}

/** Installs the listener on a window (the app's, or an email iframe's); returns the uninstall function. */
export function installShortcutListener(target: Pick<Window, 'addEventListener' | 'removeEventListener'> = window) {
  const handler = createKeydownHandler();
  target.addEventListener('keydown', handler as EventListener);
  return () => target.removeEventListener('keydown', handler as EventListener);
}

/**
 * Registers `handlers` while mounted (and `enabled`). The latest handlers object is used at
 * dispatch time, so callers can pass a fresh object every render.
 */
export function useShortcuts(handlers: ShortcutHandlers, enabled = true): void {
  const ref = useRef(handlers);
  useEffect(() => {
    ref.current = handlers;
  });
  useEffect(() => {
    if (!enabled) return;
    return registerShortcuts(() => ref.current);
  }, [enabled]);
}
