import { beforeEach, describe, expect, it } from 'vitest';
import type { StateStorage } from 'zustand/middleware';
import {
  COMPOSE_STORAGE_KEY,
  createComposeStore,
  MAX_EXPANDED_WINDOWS,
  selectExpandedWindows,
  type ComposeStore,
} from './compose';

function memoryStorage(): StateStorage & { data: Map<string, string> } {
  const data = new Map<string, string>();
  return {
    data,
    getItem: (k) => data.get(k) ?? null,
    setItem: (k, v) => void data.set(k, v),
    removeItem: (k) => void data.delete(k),
  };
}

const expandedKeys = (s: ComposeStore) => selectExpandedWindows(s).map((w) => w.key);

describe('compose store', () => {
  let storage: ReturnType<typeof memoryStorage>;
  let store: ReturnType<typeof createComposeStore>;

  beforeEach(() => {
    storage = memoryStorage();
    store = createComposeStore(() => storage);
  });

  it('opens windows with sensible defaults', () => {
    const k = store.getState().open({ kind: 'new', subject: '周报' });
    const w = store.getState().windows[0]!;
    expect(w).toMatchObject({ key: k, draftId: null, minimized: false, maximized: false, saveState: 'idle', title: '周报' });
    expect(store.getState().focusedKey).toBe(k);

    const r = store.getState().open({ kind: 'reply', parentMessageId: 5, threadId: 2 });
    expect(r).not.toBe(k);
    expect(store.getState().windows).toHaveLength(2);
  });

  it('focuses an already-open draft instead of opening a duplicate', () => {
    const a = store.getState().open({ kind: 'draft', draftId: 11 });
    const b = store.getState().open({ kind: 'new' });
    store.getState().toggleMinimize(a);
    expect(store.getState().windows.find((w) => w.key === a)!.minimized).toBe(true);

    const again = store.getState().open({ kind: 'draft', draftId: 11 });
    expect(again).toBe(a);
    expect(store.getState().windows).toHaveLength(2);
    expect(store.getState().windows.find((w) => w.key === a)!.minimized).toBe(false);
    expect(store.getState().focusedKey).toBe(a);
    expect(b).toBeTruthy();
  });

  it('also matches a draft id learned later via patch (first autosave)', () => {
    const k = store.getState().open({ kind: 'new' });
    store.getState().patch(k, { draftId: 42, saveState: 'saved' });
    expect(store.getState().open({ kind: 'draft', draftId: 42 })).toBe(k);
    expect(store.getState().windows).toHaveLength(1);
  });

  it(`keeps at most ${MAX_EXPANDED_WINDOWS} windows expanded, minimizing the least recently focused`, () => {
    const [a, b, c] = [1, 2, 3].map(() => store.getState().open({ kind: 'new' })) as [string, string, string];
    expect(expandedKeys(store.getState())).toEqual([a, b, c]);

    store.getState().focus(a); // a is now most recent; b is the oldest
    const d = store.getState().open({ kind: 'new' });
    expect(store.getState().windows).toHaveLength(4);
    expect(expandedKeys(store.getState())).toEqual([a, c, d]);
    expect(store.getState().windows.find((w) => w.key === b)!.minimized).toBe(true);

    // Restoring b minimizes the least recently focused remaining one (c).
    store.getState().toggleMinimize(b);
    expect(expandedKeys(store.getState()).sort()).toEqual([a, b, d].sort());
    expect(store.getState().windows.find((w) => w.key === c)!.minimized).toBe(true);
    expect(selectExpandedWindows(store.getState())).toHaveLength(MAX_EXPANDED_WINDOWS);
  });

  it('applies the limit when expanding through patch', () => {
    const keys = [1, 2, 3, 4].map(() => store.getState().open({ kind: 'new' }));
    const minimized = store.getState().windows.find((w) => w.minimized)!;
    expect(minimized.key).toBe(keys[0]);
    store.getState().patch(minimized.key, { minimized: false });
    expect(selectExpandedWindows(store.getState())).toHaveLength(MAX_EXPANDED_WINDOWS);
    expect(store.getState().windows.find((w) => w.key === keys[0])!.minimized).toBe(false);
  });

  it('allows a single maximized window; minimizing un-maximizes', () => {
    const a = store.getState().open({ kind: 'new' });
    const b = store.getState().open({ kind: 'new' });
    store.getState().toggleMaximize(a);
    expect(store.getState().windows.find((w) => w.key === a)!.maximized).toBe(true);
    store.getState().toggleMaximize(b);
    expect(store.getState().windows.find((w) => w.key === a)!.maximized).toBe(false);
    expect(store.getState().windows.find((w) => w.key === b)!.maximized).toBe(true);

    store.getState().toggleMinimize(b);
    const wb = store.getState().windows.find((w) => w.key === b)!;
    expect(wb).toMatchObject({ minimized: true, maximized: false });
    expect(store.getState().focusedKey).toBe(a);

    store.getState().toggleMaximize(a);
    store.getState().toggleMaximize(a);
    expect(store.getState().windows.find((w) => w.key === a)!.maximized).toBe(false);
  });

  it('focusing (or restoring) another window un-maximizes the maximized one', () => {
    const a = store.getState().open({ kind: 'new' });
    const b = store.getState().open({ kind: 'draft', draftId: 3 });
    store.getState().toggleMinimize(b);
    store.getState().toggleMaximize(a);

    store.getState().focus(b);
    const byKey = (k: string) => store.getState().windows.find((w) => w.key === k)!;
    expect(byKey(a).maximized).toBe(false);
    expect(byKey(b).minimized).toBe(false);
    expect(store.getState().focusedKey).toBe(b);

    // Same path via the chip (toggleMinimize on a minimized window) and via open() of an open draft.
    store.getState().toggleMaximize(a);
    store.getState().toggleMinimize(b);
    store.getState().toggleMinimize(b);
    expect(byKey(a).maximized).toBe(false);
    store.getState().toggleMaximize(a);
    store.getState().open({ kind: 'draft', draftId: 3 });
    expect(byKey(a).maximized).toBe(false);

    // Focusing the maximized window itself keeps it maximized.
    store.getState().toggleMaximize(a);
    store.getState().focus(a);
    expect(byKey(a).maximized).toBe(true);
  });

  it('opening a new window un-maximizes the current one', () => {
    const a = store.getState().open({ kind: 'new' });
    store.getState().toggleMaximize(a);
    store.getState().open({ kind: 'new' });
    expect(store.getState().windows.find((w) => w.key === a)!.maximized).toBe(false);
  });

  it('close removes the window and moves focus to the most recent expanded one', () => {
    const a = store.getState().open({ kind: 'new' });
    const b = store.getState().open({ kind: 'new' });
    store.getState().close(b);
    expect(store.getState().windows.map((w) => w.key)).toEqual([a]);
    expect(store.getState().focusedKey).toBe(a);
    store.getState().close('missing'); // no-op
    store.getState().close(a);
    expect(store.getState().windows).toEqual([]);
    expect(store.getState().focusedKey).toBeNull();
  });

  it('patch never changes the key and ignores unknown windows', () => {
    const a = store.getState().open({ kind: 'new' });
    store.getState().patch(a, { key: 'hijack', title: '你好', saveState: 'dirty' });
    expect(store.getState().windows[0]).toMatchObject({ key: a, title: '你好', saveState: 'dirty' });
    store.getState().patch('missing', { title: 'x' });
    expect(store.getState().windows).toHaveLength(1);
  });

  it('persists window keys + draftIds (only windows with a draft) to session storage', () => {
    const a = store.getState().open({ kind: 'draft', draftId: 7 });
    store.getState().open({ kind: 'new' }); // no draft yet → not persisted
    const c = store.getState().open({ kind: 'new' });
    store.getState().patch(c, { draftId: 8 });
    store.getState().toggleMinimize(c);

    const raw = storage.data.get(COMPOSE_STORAGE_KEY);
    expect(raw).toBeTruthy();
    const parsed = JSON.parse(raw!) as { state: { windows: unknown[] }; version: number };
    expect(parsed.version).toBe(1);
    expect(parsed.state.windows).toEqual([
      { key: a, draftId: 7, minimized: false },
      { key: c, draftId: 8, minimized: true },
    ]);
  });

  it('restores persisted windows as draft windows after a reload', () => {
    const a = store.getState().open({ kind: 'draft', draftId: 7 });
    const b = store.getState().open({ kind: 'new' });
    store.getState().patch(b, { draftId: 9, title: '会议纪要', saveState: 'saved' });
    store.getState().toggleMinimize(b);

    const reloaded = createComposeStore(() => storage);
    const ws = reloaded.getState().windows;
    expect(ws.map((w) => w.key)).toEqual([a, b]);
    expect(ws[0]).toMatchObject({ draftId: 7, init: { kind: 'draft', draftId: 7 }, minimized: false, maximized: false });
    expect(ws[1]).toMatchObject({ draftId: 9, init: { kind: 'draft', draftId: 9 }, minimized: true, saveState: 'saved' });
    expect(reloaded.getState().focusedKey).toBe(a);
    // The restored store keeps the dedupe rule.
    expect(reloaded.getState().open({ kind: 'draft', draftId: 9 })).toBe(b);
  });

  it('tolerates corrupt or duplicated persisted data', () => {
    storage.data.set(
      COMPOSE_STORAGE_KEY,
      JSON.stringify({
        version: 1,
        state: {
          windows: [
            { key: 'k1', draftId: 1 },
            { key: 'k2', draftId: 1 },
            { key: 3, draftId: 2 },
            null,
            { key: 'k4', draftId: 'x' },
            ...[5, 6, 7, 8].map((n) => ({ key: `n${n}`, draftId: n, minimized: false })),
          ],
        },
      }),
    );
    const s = createComposeStore(() => storage).getState();
    expect(s.windows.map((w) => w.key)).toEqual(['k1', 'n5', 'n6', 'n7', 'n8']);
    expect(selectExpandedWindows(s)).toHaveLength(MAX_EXPANDED_WINDOWS);

    storage.data.set(COMPOSE_STORAGE_KEY, '{not json');
    expect(createComposeStore(() => storage).getState().windows).toEqual([]);
  });

  it('setOwner: adopts the first user, keeps windows for the same user, closes them for another', () => {
    store.getState().open({ kind: 'draft', draftId: 1 });
    store.getState().setOwner(10);
    expect(store.getState().ownerId).toBe(10);
    expect(store.getState().windows).toHaveLength(1);

    store.getState().setOwner(10);
    expect(store.getState().windows).toHaveLength(1);

    store.getState().setOwner(20);
    expect(store.getState().ownerId).toBe(20);
    expect(store.getState().windows).toEqual([]);
    expect(store.getState().focusedKey).toBeNull();
  });

  it('persists and restores ownerId, so a reload still detects a different user', () => {
    store.getState().setOwner(10);
    store.getState().open({ kind: 'draft', draftId: 4 });
    expect(JSON.parse(storage.data.get(COMPOSE_STORAGE_KEY)!).state.ownerId).toBe(10);

    const reloaded = createComposeStore(() => storage);
    expect(reloaded.getState().ownerId).toBe(10);
    expect(reloaded.getState().windows).toHaveLength(1);
    reloaded.getState().setOwner(11);
    expect(reloaded.getState().windows).toEqual([]);
  });

  it('closeAll empties the store and its persisted copy', () => {
    store.getState().open({ kind: 'draft', draftId: 1 });
    store.getState().closeAll();
    expect(store.getState().windows).toEqual([]);
    expect(JSON.parse(storage.data.get(COMPOSE_STORAGE_KEY)!).state.windows).toEqual([]);
  });
});
