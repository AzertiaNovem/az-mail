/**
 * Mail UI state [WP-E] (zustand): sidebar, density, thread selection, the keyboard cursor and
 * the current list (for j/k and "第 3 封，共 50 封" in the thread view), the Gmail pager's cursor
 * stacks, the shortcuts help dialog and search-box focus requests.
 *
 * Only `sidebarCollapsed` and `density` persist (localStorage `azmail.ui`); everything else is
 * per page load. Selection and cursor are scoped to one list (`scope` = a stable key of the
 * list view, see `listScope()`), so switching folders never acts on a stale selection.
 */
import { create } from 'zustand';
import { createJSONStorage, persist, type StateStorage } from 'zustand/middleware';
import type { ThreadListFilter } from '@/api/queryKeys';

export type Density = 'default' | 'comfortable' | 'compact';

export const UI_STORAGE_KEY = 'azmail.ui';

/** Stable key of a list view (folder / label / search), independent of the page. */
export function listScope(f: ThreadListFilter): string {
  if (f.q !== null) return `q:${f.q}`;
  if (f.labelId !== null) return `label:${f.labelId}`;
  return `folder:${f.folder ?? 'inbox'}`;
}

export interface ListContext {
  scope: string;
  /** Thread ids of the visible page, in order. */
  ids: number[];
  /** Base path of the list, e.g. "/mail/inbox", "/mail/label/3", "/mail/search". */
  basePath: string;
  /** Query string to keep ("?q=…" for search, else ""). */
  search: string;
  /** 0-based index of the page's first row in the whole list. */
  offset: number;
  /** Total threads in the list when known. */
  total: number | null;
}

export interface UiState {
  sidebarCollapsed: boolean;
  /** Drawer state on narrow screens (sidebar hidden by default). */
  mobileNavOpen: boolean;
  density: Density;

  selectionScope: string | null;
  selected: number[];
  /** Last checkbox toggled (anchor for shift-click ranges). */
  selectionAnchor: number | null;

  list: ListContext | null;
  /** Keyboard cursor (thread id) in the current list. */
  cursorId: number | null;

  /** Pager cursor stack per list scope: [] = page 1; the last element is the current page's cursor. */
  pages: Record<string, string[]>;

  shortcutsHelpOpen: boolean;
  /** Incremented to ask the search box to take focus (shortcut "/"). */
  searchFocusTick: number;

  toggleSidebar(): void;
  setSidebarCollapsed(v: boolean): void;
  setMobileNavOpen(v: boolean): void;
  setDensity(d: Density): void;

  /** Replaces the selection of `scope`. */
  setSelection(scope: string, ids: number[]): void;
  /** Toggles one id (or sets it to `on`) in `scope`'s selection. */
  toggleSelected(scope: string, id: number, on?: boolean): void;
  /** Selects/deselects the range between the anchor and `id` within `orderedIds`. */
  selectRange(scope: string, orderedIds: number[], id: number, on: boolean): void;
  clearSelection(): void;
  /** Drops selected ids that are no longer visible (after a refetch / optimistic removal). */
  pruneSelection(scope: string, visibleIds: number[]): void;

  setList(ctx: ListContext): void;
  setCursor(id: number | null): void;

  pushPage(scope: string, cursor: string): void;
  popPage(scope: string): void;
  resetPages(scope: string): void;

  setShortcutsHelpOpen(v: boolean): void;
  requestSearchFocus(): void;
}

const localStorageOrNoop = (): StateStorage => {
  try {
    if (typeof localStorage !== 'undefined') return localStorage;
  } catch {
    /* storage disabled */
  }
  return { getItem: () => null, setItem: () => {}, removeItem: () => {} };
};

const sameIds = (a: number[], b: number[]) => a.length === b.length && a.every((x, i) => x === b[i]);

export function createUiStore(storage: () => StateStorage = localStorageOrNoop) {
  return create<UiState>()(
    persist(
      (set, get) => ({
        sidebarCollapsed: false,
        mobileNavOpen: false,
        density: 'default',
        selectionScope: null,
        selected: [],
        selectionAnchor: null,
        list: null,
        cursorId: null,
        pages: {},
        shortcutsHelpOpen: false,
        searchFocusTick: 0,

        toggleSidebar: () => set((s) => ({ sidebarCollapsed: !s.sidebarCollapsed })),
        setSidebarCollapsed: (v) => set({ sidebarCollapsed: v }),
        setMobileNavOpen: (v) => set({ mobileNavOpen: v }),
        setDensity: (d) => set({ density: d }),

        setSelection: (scope, ids) =>
          set({ selectionScope: scope, selected: [...new Set(ids)], selectionAnchor: ids.length ? (ids[ids.length - 1] ?? null) : null }),

        toggleSelected: (scope, id, on) => {
          const s = get();
          const current = s.selectionScope === scope ? s.selected : [];
          const has = current.includes(id);
          const want = on ?? !has;
          if (want === has && s.selectionScope === scope) {
            set({ selectionAnchor: id });
            return;
          }
          set({
            selectionScope: scope,
            selected: want ? [...current, id] : current.filter((x) => x !== id),
            selectionAnchor: id,
          });
        },

        selectRange: (scope, orderedIds, id, on) => {
          const s = get();
          const current = s.selectionScope === scope ? s.selected : [];
          const anchor = s.selectionScope === scope ? s.selectionAnchor : null;
          const a = anchor === null ? -1 : orderedIds.indexOf(anchor);
          const b = orderedIds.indexOf(id);
          if (b === -1) return;
          const [from, to] = a === -1 ? [b, b] : a < b ? [a, b] : [b, a];
          const range = new Set(orderedIds.slice(from, to + 1));
          const next = on ? [...new Set([...current, ...range])] : current.filter((x) => !range.has(x));
          set({ selectionScope: scope, selected: next, selectionAnchor: id });
        },

        clearSelection: () => {
          if (get().selected.length || get().selectionAnchor !== null) set({ selected: [], selectionAnchor: null });
        },

        pruneSelection: (scope, visibleIds) => {
          const s = get();
          if (s.selectionScope !== scope || s.selected.length === 0) return;
          const visible = new Set(visibleIds);
          const next = s.selected.filter((x) => visible.has(x));
          if (next.length !== s.selected.length) set({ selected: next });
        },

        setList: (ctx) => {
          const prev = get().list;
          if (
            prev &&
            prev.scope === ctx.scope &&
            prev.basePath === ctx.basePath &&
            prev.search === ctx.search &&
            prev.offset === ctx.offset &&
            prev.total === ctx.total &&
            sameIds(prev.ids, ctx.ids)
          )
            return;
          const cursor = get().cursorId;
          let next: number | null = null;
          if (cursor !== null) {
            if (ctx.ids.includes(cursor)) next = cursor;
            else if (prev && prev.scope === ctx.scope && prev.offset === ctx.offset) {
              // The cursor row left the page (archived, deleted…): move to the row that took its place.
              const i = prev.ids.indexOf(cursor);
              if (i !== -1 && ctx.ids.length) next = ctx.ids[Math.min(i, ctx.ids.length - 1)] ?? null;
            }
          }
          set({ list: ctx, cursorId: next });
        },

        setCursor: (id) => set({ cursorId: id }),

        pushPage: (scope, cursor) => set((s) => ({ pages: { ...s.pages, [scope]: [...(s.pages[scope] ?? []), cursor] } })),
        popPage: (scope) => set((s) => ({ pages: { ...s.pages, [scope]: (s.pages[scope] ?? []).slice(0, -1) } })),
        resetPages: (scope) => {
          if ((get().pages[scope] ?? []).length === 0) return;
          set((s) => ({ pages: { ...s.pages, [scope]: [] } }));
        },

        setShortcutsHelpOpen: (v) => set({ shortcutsHelpOpen: v }),
        requestSearchFocus: () => set((s) => ({ searchFocusTick: s.searchFocusTick + 1 })),
      }),
      {
        name: UI_STORAGE_KEY,
        version: 1,
        storage: createJSONStorage(storage),
        partialize: (s) => ({ sidebarCollapsed: s.sidebarCollapsed, density: s.density }),
        merge: (persisted, current) => {
          const p = (persisted ?? {}) as Partial<Pick<UiState, 'sidebarCollapsed' | 'density'>>;
          return {
            ...current,
            sidebarCollapsed: typeof p.sidebarCollapsed === 'boolean' ? p.sidebarCollapsed : current.sidebarCollapsed,
            density: p.density === 'comfortable' || p.density === 'compact' || p.density === 'default' ? p.density : current.density,
          };
        },
      },
    ),
  );
}

export const useUiStore = createUiStore();

/** The selection of `scope` (empty when another list owns the selection). */
export const selectSelection =
  (scope: string) =>
  (s: UiState): number[] =>
    s.selectionScope === scope ? s.selected : EMPTY;

const EMPTY: number[] = [];

/** Current page cursor of `scope` (null = first page). */
export const selectPageCursor =
  (scope: string) =>
  (s: UiState): string | null => {
    const stack = s.pages[scope];
    return stack && stack.length ? (stack[stack.length - 1] ?? null) : null;
  };

/** 0-based page index of `scope`. */
export const selectPageIndex =
  (scope: string) =>
  (s: UiState): number =>
    s.pages[scope]?.length ?? 0;
