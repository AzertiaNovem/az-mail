import { QueryClient } from '@tanstack/react-query';
import { afterEach, describe, expect, it } from 'vitest';
import type { Me } from '@/api/types';
import {
  clearToken,
  completeLogin,
  getCachedMe,
  getToken,
  installAuthStorageSync,
  installComposeOwnerSync,
  setCachedMe,
  logoutLocally,
  onExternalLogin,
  onUnauthorized,
  setToken,
  TOKEN_KEY,
} from './auth';
import { useComposeStore } from './compose';

const me = { id: 1, email: 'alice@example.com', display_name: 'Alice' } as Me;

function storageEvent(key: string | null, newValue: string | null) {
  window.dispatchEvent(new StorageEvent('storage', { key, newValue }));
}

describe('auth store', () => {
  const cleanups: Array<() => void> = [];
  afterEach(() => {
    while (cleanups.length) cleanups.pop()!();
    clearToken();
    useComposeStore.setState({ windows: [], focusOrder: [], focusedKey: null, ownerId: null });
  });

  it('stores the token in localStorage under azmail.token', () => {
    setToken('abc');
    expect(localStorage.getItem(TOKEN_KEY)).toBe('abc');
    expect(getToken()).toBe('abc');
    clearToken();
    expect(localStorage.getItem(TOKEN_KEY)).toBeNull();
    expect(getToken()).toBeNull();
  });

  it('completeLogin primes ["me"]; logoutLocally clears caches and emits logout', () => {
    const qc = new QueryClient();
    qc.setQueryData(['threads', 'x'], 1);
    completeLogin(qc, { token: 't1', expires_at: 0, user: me });
    expect(getToken()).toBe('t1');
    expect(getCachedMe(qc)).toEqual(me);
    expect(qc.getQueryData(['threads', 'x'])).toBeUndefined();

    const reasons: string[] = [];
    cleanups.push(onUnauthorized((r) => reasons.push(r)));
    logoutLocally(qc);
    expect(getToken()).toBeNull();
    expect(getCachedMe(qc)).toBeUndefined();
    expect(reasons).toEqual(['logout']);
  });

  it('completeLogin keeps compose windows for the same user and closes them for another', () => {
    const qc = new QueryClient();
    completeLogin(qc, { token: 't1', expires_at: 0, user: me });
    useComposeStore.getState().open({ kind: 'draft', draftId: 5 });

    // Session expired (windows are kept on purpose), then the same user logs back in.
    clearToken();
    completeLogin(qc, { token: 't2', expires_at: 0, user: me });
    expect(useComposeStore.getState().windows).toHaveLength(1);

    // Someone else logs in on this tab: alice's draft windows must not survive.
    completeLogin(qc, { token: 't3', expires_at: 0, user: { ...me, id: 2, email: 'bob@example.com' } });
    expect(useComposeStore.getState().windows).toEqual([]);
    expect(useComposeStore.getState().ownerId).toBe(2);
  });

  it('installComposeOwnerSync applies the owner check whenever ["me"] loads', () => {
    const qc = new QueryClient();
    cleanups.push(installComposeOwnerSync(qc));
    setCachedMe(qc, me);
    expect(useComposeStore.getState().ownerId).toBe(1);
    useComposeStore.getState().open({ kind: 'draft', draftId: 8 });
    qc.setQueryData(['threads', 'x'], 1); // unrelated keys are ignored
    expect(useComposeStore.getState().windows).toHaveLength(1);
    setCachedMe(qc, { ...me, id: 3 }); // e.g. refetch after another tab logged in as someone else
    expect(useComposeStore.getState().windows).toEqual([]);
  });

  it('syncs logout and login from other tabs via the storage event', () => {
    cleanups.push(installAuthStorageSync(window));
    const reasons: string[] = [];
    const logins: string[] = [];
    cleanups.push(onUnauthorized((r) => reasons.push(r)));
    cleanups.push(onExternalLogin((t) => logins.push(t)));

    setToken('mine');
    storageEvent('unrelated', 'x');
    expect(getToken()).toBe('mine');

    storageEvent(TOKEN_KEY, 'theirs');
    expect(getToken()).toBe('theirs');
    expect(logins).toEqual(['theirs']);

    storageEvent(TOKEN_KEY, null);
    expect(getToken()).toBeNull();
    expect(reasons).toEqual(['other_tab']);

    setToken('again');
    storageEvent(null, null); // localStorage.clear() elsewhere
    expect(getToken()).toBeNull();
    expect(reasons).toEqual(['other_tab', 'other_tab']);
  });
});
