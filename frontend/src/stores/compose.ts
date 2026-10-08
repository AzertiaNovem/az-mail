/**
 * Compose window manager (DESIGN.md §5 "Compose store", contract).
 *
 * Rules:
 * - `open({kind:'draft', draftId})` for a draft that is already open focuses that window.
 * - At most MAX_EXPANDED_WINDOWS (3) windows are expanded; opening / focusing / restoring one
 *   minimizes the least recently focused others (they become chips in the dock).
 * - `windows` is in open order; the dock lays them out right-to-left (windows[0] rightmost).
 * - Only one window is maximized at a time; minimizing a window also un-maximizes it, and
 *   opening / focusing another window un-maximizes the maximized one (it would cover it).
 * - Window keys + draftIds persist in sessionStorage (`azmail.compose`); after a reload, windows
 *   that have a saved draft come back as `{kind:'draft', draftId}`. Windows without a draft yet
 *   (nothing typed — drafts are created lazily on first edit) are not restored.
 * - `ownerId` (persisted) is the user whose drafts the windows hold. `setOwner(id)` with a
 *   different user closes every window: after an expired session another person may log in on
 *   the same tab, and the previous user's draftIds would 404 for them. auth.ts calls it on login
 *   and whenever `['me']` loads (`installComposeOwnerSync`).
 *
 * Editor content is not stored here: it lives in TipTap and is saved by the window's autosave.
 * `focusedKey`, `focusOrder`, `closeAll`, `ownerId` and `setOwner` are additive extensions of the contract,
 * as are `cc` / `bcc` / `body` of a new message (mailto: links) and `ComposeWin.unsaved`.
 *
 * A window's form can unmount while the window stays open: when the session expires the app
 * shell unmounts on /login and remounts after the same user logs back in. The remounted form
 * then loads the window's saved draft (`draftId`, not `init`), and `unsaved` carries the edits
 * that had not reached the server yet (the form stashes them on unmount).
 */
import { create } from 'zustand';
import { createJSONStorage, persist, type StateStorage } from 'zustand/middleware';
import type { Address, Attachment } from '@/api/types';

export type ComposeInit =
  | { kind: 'new'; to?: Address[]; cc?: Address[]; bcc?: Address[]; subject?: string; /** Plain text (mailto: body). */ body?: string }
  | { kind: 'reply' | 'reply_all' | 'forward'; parentMessageId: number; threadId: number }
  | { kind: 'draft'; draftId: number };

export type ComposeSaveState = 'idle' | 'dirty' | 'saving' | 'saved' | 'error' | 'conflict';

/** Field values of a form that unmounted with edits the server has not stored yet (in memory only). */
export interface ComposeUnsaved {
  fromAddressId: number | undefined;
  to: Address[];
  cc: Address[];
  bcc: Address[];
  subject: string;
  html: string;
  quotedHtml: string | null;
  attachments: Attachment[];
  /** Inline attachments owned by the editor body (see attachmentIdsForSave). */
  editorInlineIds: number[];
}

export interface ComposeWin {
  key: string;
  draftId: number | null;
  init: ComposeInit;
  minimized: boolean;
  maximized: boolean;
  saveState: ComposeSaveState;
  title: string;
  /** Additive: unsaved edits of a form that unmounted while the window stayed open. */
  unsaved?: ComposeUnsaved;
}

export interface ComposeStore {
  windows: ComposeWin[];
  /** Opens a window (or focuses the one already showing that draft); returns its key. */
  open(i: ComposeInit): string;
  close(k: string): void;
  toggleMinimize(k: string): void;
  toggleMaximize(k: string): void;
  /** Brings a window to front, expanding it if minimized. */
  focus(k: string): void;
  /** Merges fields into a window (`key` is immutable). Setting `minimized:false` applies the 3-window rule. */
  patch(k: string, p: Partial<ComposeWin>): void;

  // ── additive ──
  /** Most recently focused window, or null. */
  focusedKey: string | null;
  /** Window keys from least to most recently focused. */
  focusOrder: string[];
  /** Closes everything (e.g. on logout). Keeps `ownerId`. */
  closeAll(): void;
  /** User id the open windows belong to (null until first set). */
  ownerId: number | null;
  /** Records the signed-in user; a different user than `ownerId` closes every window first. */
  setOwner(userId: number): void;
}

export const MAX_EXPANDED_WINDOWS = 3;
export const COMPOSE_STORAGE_KEY = 'azmail.compose';

/** What survives a reload. */
interface PersistedWin {
  key: string;
  draftId: number;
  minimized: boolean;
}
interface PersistedState {
  windows: PersistedWin[];
  ownerId: number | null;
}

let keySeq = 0;
const newKey = (): string => `cw_${Date.now().toString(36)}_${(keySeq++).toString(36)}`;

function initialTitle(init: ComposeInit): string {
  return init.kind === 'new' ? (init.subject ?? '') : '';
}

/** Moves `key` to the most-recent end of the focus order. */
function touch(order: string[], key: string): string[] {
  return [...order.filter((k) => k !== key), key];
}

/**
 * Minimizes the least recently focused expanded windows (never `keep`) until at most
 * MAX_EXPANDED_WINDOWS remain expanded.
 */
function enforceExpandedLimit(windows: ComposeWin[], order: string[], keep: string | null): ComposeWin[] {
  const expanded = windows.filter((w) => !w.minimized);
  let excess = expanded.length - MAX_EXPANDED_WINDOWS;
  if (excess <= 0) return windows;
  const rank = (k: string) => order.indexOf(k); // -1 = never focused = oldest
  const victims = new Set<string>();
  for (const w of [...expanded].sort((a, b) => rank(a.key) - rank(b.key))) {
    if (excess <= 0) break;
    if (w.key === keep) continue;
    victims.add(w.key);
    excess--;
  }
  return windows.map((w) => (victims.has(w.key) ? { ...w, minimized: true, maximized: false } : w));
}

/** The most recently focused window that is still open and expanded. */
function pickFocused(windows: ComposeWin[], order: string[]): string | null {
  for (let i = order.length - 1; i >= 0; i--) {
    const k = order[i];
    if (windows.some((w) => w.key === k && !w.minimized)) return k ?? null;
  }
  return null;
}

function restoreOwner(persisted: unknown): number | null {
  const v = (persisted as Partial<PersistedState> | null | undefined)?.ownerId;
  return typeof v === 'number' && Number.isFinite(v) ? v : null;
}

function restoreWindows(persisted: unknown): PersistedWin[] {
  const list = (persisted as Partial<PersistedState> | null | undefined)?.windows;
  if (!Array.isArray(list)) return [];
  const seen = new Set<number>();
  const out: PersistedWin[] = [];
  for (const raw of list as unknown[]) {
    const w = raw as Partial<PersistedWin> | null;
    if (!w || typeof w.key !== 'string' || typeof w.draftId !== 'number' || !Number.isFinite(w.draftId)) continue;
    if (seen.has(w.draftId)) continue; // one window per draft
    seen.add(w.draftId);
    out.push({ key: w.key, draftId: w.draftId, minimized: w.minimized === true });
  }
  return out;
}

const sessionStorageOrNoop = (): StateStorage => {
  try {
    if (typeof sessionStorage !== 'undefined') return sessionStorage;
  } catch {
    /* storage disabled */
  }
  return { getItem: () => null, setItem: () => {}, removeItem: () => {} };
};

/** Store factory (tests pass their own storage). */
export function createComposeStore(storage: () => StateStorage = sessionStorageOrNoop) {
  return create<ComposeStore>()(
    persist<ComposeStore, [], [], PersistedState>(
      (set, get) => {
        const focus = (k: string) => {
          const s = get();
          if (!s.windows.some((w) => w.key === k)) return;
          const order = touch(s.focusOrder, k);
          // Expand the target; un-maximize any other window, which would otherwise cover it.
          const windows = s.windows.map((w) =>
            w.key === k ? { ...w, minimized: false } : w.maximized ? { ...w, maximized: false } : w,
          );
          set({ windows: enforceExpandedLimit(windows, order, k), focusOrder: order, focusedKey: k });
        };

        return {
          windows: [],
          focusedKey: null,
          focusOrder: [],
          ownerId: null,

          open(init) {
            if (init.kind === 'draft') {
              const existing = get().windows.find((w) => w.draftId === init.draftId);
              if (existing) {
                focus(existing.key);
                return existing.key;
              }
            }
            const key = newKey();
            const win: ComposeWin = {
              key,
              draftId: init.kind === 'draft' ? init.draftId : null,
              init,
              minimized: false,
              maximized: false,
              saveState: init.kind === 'draft' ? 'saved' : 'idle',
              title: initialTitle(init),
            };
            const s = get();
            // A maximized window would cover the new one.
            const windows = [...s.windows.map((w) => (w.maximized ? { ...w, maximized: false } : w)), win];
            const order = touch(s.focusOrder, key);
            set({ windows: enforceExpandedLimit(windows, order, key), focusOrder: order, focusedKey: key });
            return key;
          },

          close(k) {
            const s = get();
            if (!s.windows.some((w) => w.key === k)) return;
            const windows = s.windows.filter((w) => w.key !== k);
            const order = s.focusOrder.filter((x) => x !== k);
            set({ windows, focusOrder: order, focusedKey: pickFocused(windows, order) });
          },

          toggleMinimize(k) {
            const w = get().windows.find((x) => x.key === k);
            if (!w) return;
            if (w.minimized) {
              focus(k);
              return;
            }
            const s = get();
            const windows = s.windows.map((x) => (x.key === k ? { ...x, minimized: true, maximized: false } : x));
            set({ windows, focusedKey: pickFocused(windows, s.focusOrder) });
          },

          toggleMaximize(k) {
            const s = get();
            const w = s.windows.find((x) => x.key === k);
            if (!w) return;
            if (w.maximized) {
              set({ windows: s.windows.map((x) => (x.key === k ? { ...x, maximized: false } : x)) });
              return;
            }
            const order = touch(s.focusOrder, k);
            const windows = s.windows.map((x) =>
              x.key === k ? { ...x, maximized: true, minimized: false } : x.maximized ? { ...x, maximized: false } : x,
            );
            set({ windows: enforceExpandedLimit(windows, order, k), focusOrder: order, focusedKey: k });
          },

          focus,

          patch(k, p) {
            const s = get();
            if (!s.windows.some((w) => w.key === k)) return;
            const { key: _ignored, ...rest } = p;
            void _ignored;
            let windows = s.windows.map((w) => (w.key === k ? { ...w, ...rest } : w));
            let order = s.focusOrder;
            let focusedKey = s.focusedKey;
            if (rest.minimized === false) {
              order = touch(order, k);
              windows = enforceExpandedLimit(windows, order, k);
              focusedKey = k;
            } else if (rest.minimized === true) {
              windows = windows.map((w) => (w.key === k ? { ...w, maximized: false } : w));
              focusedKey = pickFocused(windows, order);
            }
            if (rest.maximized === true) {
              windows = windows.map((w) => (w.key !== k && w.maximized ? { ...w, maximized: false } : w));
            }
            set({ windows, focusOrder: order, focusedKey });
          },

          closeAll() {
            set({ windows: [], focusOrder: [], focusedKey: null });
          },

          setOwner(userId) {
            const s = get();
            if (s.ownerId === userId) return;
            if (s.ownerId !== null && s.windows.length > 0) {
              set({ windows: [], focusOrder: [], focusedKey: null, ownerId: userId });
              return;
            }
            set({ ownerId: userId });
          },
        };
      },
      {
        name: COMPOSE_STORAGE_KEY,
        version: 1,
        storage: createJSONStorage<PersistedState>(storage),
        partialize: (s) => ({
          ownerId: s.ownerId,
          windows: s.windows
            .filter((w): w is ComposeWin & { draftId: number } => w.draftId !== null)
            .map((w) => ({ key: w.key, draftId: w.draftId, minimized: w.minimized })),
        }),
        merge: (persisted, current) => {
          const ownerId = restoreOwner(persisted);
          const restored = restoreWindows(persisted);
          if (restored.length === 0) return { ...current, ownerId };
          const windows: ComposeWin[] = restored.map((p) => ({
            key: p.key,
            draftId: p.draftId,
            init: { kind: 'draft', draftId: p.draftId },
            minimized: p.minimized,
            maximized: false,
            saveState: 'saved',
            title: '',
          }));
          const order = windows.map((w) => w.key);
          const limited = enforceExpandedLimit(windows, order, null);
          return { ...current, ownerId, windows: limited, focusOrder: order, focusedKey: pickFocused(limited, order) };
        },
      },
    ),
  );
}

export const useComposeStore = createComposeStore();

// ── selectors ──
export const selectExpandedWindows = (s: ComposeStore): ComposeWin[] => s.windows.filter((w) => !w.minimized);
export const selectMinimizedWindows = (s: ComposeStore): ComposeWin[] => s.windows.filter((w) => w.minimized);
export const selectWindow =
  (key: string) =>
  (s: ComposeStore): ComposeWin | undefined =>
    s.windows.find((w) => w.key === key);
