import { describe, expect, it } from 'vitest';
import { createUiStore, listScope, selectPageCursor, selectPageIndex, selectSelection, UI_STORAGE_KEY } from './ui';

function memoryStorage(initial: Record<string, string> = {}) {
  const data = new Map(Object.entries(initial));
  return { getItem: (k: string) => data.get(k) ?? null, setItem: (k: string, v: string) => void data.set(k, v), removeItem: (k: string) => void data.delete(k), data };
}

const ctx = (ids: number[], over = {}) => ({ scope: 'folder:inbox', ids, basePath: '/mail/inbox', search: '', offset: 0, total: ids.length, ...over });

describe('ui store', () => {
  it('derives stable list scopes', () => {
    expect(listScope({ folder: 'inbox', labelId: null, q: null })).toBe('folder:inbox');
    expect(listScope({ folder: null, labelId: 3, q: null })).toBe('label:3');
    expect(listScope({ folder: null, labelId: null, q: 'from:a' })).toBe('q:from:a');
  });

  it('scopes the selection to one list and supports shift ranges', () => {
    const s = createUiStore(() => memoryStorage());
    const st = () => s.getState();
    st().toggleSelected('folder:inbox', 2);
    st().toggleSelected('folder:inbox', 4);
    expect(selectSelection('folder:inbox')(st())).toEqual([2, 4]);
    expect(selectSelection('folder:sent')(st())).toEqual([]);
    st().toggleSelected('folder:inbox', 2);
    expect(selectSelection('folder:inbox')(st())).toEqual([4]);
    // Range from the anchor (2, last toggled) to 5.
    st().selectRange('folder:inbox', [1, 2, 3, 4, 5, 6], 5, true);
    expect(selectSelection('folder:inbox')(st()).sort()).toEqual([2, 3, 4, 5]);
    st().selectRange('folder:inbox', [1, 2, 3, 4, 5, 6], 3, false);
    expect(selectSelection('folder:inbox')(st()).sort()).toEqual([2]);
    // Selecting in another list starts over.
    st().toggleSelected('folder:sent', 9);
    expect(selectSelection('folder:inbox')(st())).toEqual([]);
    expect(selectSelection('folder:sent')(st())).toEqual([9]);
    st().pruneSelection('folder:sent', [1, 2]);
    expect(st().selected).toEqual([]);
    st().setSelection('folder:inbox', [1, 1, 2]);
    expect(st().selected).toEqual([1, 2]);
    st().clearSelection();
    expect(st().selected).toEqual([]);
  });

  it('keeps the keyboard cursor on the row that replaces a removed one', () => {
    const s = createUiStore(() => memoryStorage());
    s.getState().setList(ctx([1, 2, 3, 4]));
    s.getState().setCursor(2);
    s.getState().setList(ctx([1, 3, 4]));
    expect(s.getState().cursorId).toBe(3);
    s.getState().setCursor(4);
    s.getState().setList(ctx([1, 3]));
    expect(s.getState().cursorId).toBe(3);
    // Another page or list: the cursor is dropped.
    s.getState().setList(ctx([7, 8], { offset: 50 }));
    expect(s.getState().cursorId).toBeNull();
  });

  it('keeps a cursor stack per list (Gmail pager)', () => {
    const s = createUiStore(() => memoryStorage());
    const st = () => s.getState();
    expect(selectPageCursor('folder:inbox')(st())).toBeNull();
    st().pushPage('folder:inbox', 'c50');
    st().pushPage('folder:inbox', 'c100');
    expect(selectPageCursor('folder:inbox')(st())).toBe('c100');
    expect(selectPageIndex('folder:inbox')(st())).toBe(2);
    expect(selectPageIndex('folder:sent')(st())).toBe(0);
    st().popPage('folder:inbox');
    expect(selectPageCursor('folder:inbox')(st())).toBe('c50');
    st().resetPages('folder:inbox');
    expect(selectPageCursor('folder:inbox')(st())).toBeNull();
  });

  it('persists only the sidebar and density preferences', () => {
    const storage = memoryStorage();
    const s = createUiStore(() => storage);
    s.getState().toggleSidebar();
    s.getState().setDensity('compact');
    s.getState().toggleSelected('folder:inbox', 1);
    s.getState().requestSearchFocus();
    const saved = JSON.parse(storage.data.get(UI_STORAGE_KEY)!);
    expect(saved.state).toEqual({ sidebarCollapsed: true, density: 'compact' });

    const restored = createUiStore(() => storage);
    expect(restored.getState().sidebarCollapsed).toBe(true);
    expect(restored.getState().density).toBe('compact');
    expect(restored.getState().selected).toEqual([]);

    const junk = createUiStore(() => memoryStorage({ [UI_STORAGE_KEY]: JSON.stringify({ state: { sidebarCollapsed: 'yes', density: 'huge' }, version: 1 }) }));
    expect(junk.getState().sidebarCollapsed).toBe(false);
    expect(junk.getState().density).toBe('default');
  });
});
