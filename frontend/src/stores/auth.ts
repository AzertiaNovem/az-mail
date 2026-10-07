/**
 * Session token storage + auth events (DESIGN.md §1 E4).
 *
 * The bearer token lives in localStorage under `azmail.token`; every tab shares it, and a
 * `storage` event propagates logout (and logins) across tabs. The current user (`Me`) is cached
 * in TanStack Query under `['me']`; the helpers below keep both in step.
 *
 * This module must not import the API client (the client imports it).
 */
import { hashKey, type QueryClient } from '@tanstack/react-query';
import { create } from 'zustand';
import { queryKeys } from '@/api/queryKeys';
import type { LoginResponse, Me } from '@/api/types';
import { useComposeStore } from './compose';

export const TOKEN_KEY = 'azmail.token';

/** Why the session ended: API 401, WS `session.revoked`, explicit logout, or another tab. */
export type UnauthorizedReason = 'expired' | 'revoked' | 'logout' | 'other_tab';

function readStoredToken(): string | null {
  try {
    return localStorage.getItem(TOKEN_KEY);
  } catch {
    return null; // storage disabled
  }
}

interface AuthState {
  token: string | null;
}

export const useAuthStore = create<AuthState>(() => ({ token: readStoredToken() }));

export const getToken = (): string | null => useAuthStore.getState().token;

/** React hook: whether a token is present (validity is established by loading `['me']`). */
export const useIsAuthenticated = (): boolean => useAuthStore((s) => s.token !== null);

export function setToken(token: string): void {
  try {
    localStorage.setItem(TOKEN_KEY, token);
  } catch {
    /* keep the in-memory token */
  }
  useAuthStore.setState({ token });
}

export function clearToken(): void {
  try {
    localStorage.removeItem(TOKEN_KEY);
  } catch {
    /* ignore */
  }
  useAuthStore.setState({ token: null });
}

// ───────────── events ─────────────

type UnauthorizedListener = (reason: UnauthorizedReason) => void;
type ExternalLoginListener = (token: string) => void;

const unauthorizedListeners = new Set<UnauthorizedListener>();
const externalLoginListeners = new Set<ExternalLoginListener>();

/** Subscribe to "session ended" (the app clears caches and redirects to /login). Returns unsubscribe. */
export function onUnauthorized(listener: UnauthorizedListener): () => void {
  unauthorizedListeners.add(listener);
  return () => unauthorizedListeners.delete(listener);
}

/** Subscribe to "another tab stored a different token" (the app should reset its caches). */
export function onExternalLogin(listener: ExternalLoginListener): () => void {
  externalLoginListeners.add(listener);
  return () => externalLoginListeners.delete(listener);
}

/** Clears the token (if still present) and notifies listeners. */
export function emitUnauthorized(reason: UnauthorizedReason): void {
  if (getToken() !== null) clearToken();
  for (const l of [...unauthorizedListeners]) {
    try {
      l(reason);
    } catch (e) {
      console.error('unauthorized listener failed', e);
    }
  }
}

/**
 * Called by the API client on HTTP 401 for an authenticated request. Ignored when the token
 * has changed since the request was sent (e.g. a fresh login raced with a stale request).
 */
export function handleUnauthorizedResponse(tokenUsed: string): void {
  if (getToken() === tokenUsed) emitUnauthorized('expired');
}

// ───────────── `Me` cache helpers ─────────────

export function getCachedMe(qc: QueryClient): Me | undefined {
  return qc.getQueryData<Me>(queryKeys.me());
}

export function setCachedMe(qc: QueryClient, me: Me): void {
  qc.setQueryData<Me>(queryKeys.me(), me);
}

/**
 * Store a successful login: token + primed `['me']` (drops anything cached for a previous user).
 * Compose windows kept across an expired session survive only if the same user logs back in.
 */
export function completeLogin(qc: QueryClient, res: LoginResponse): void {
  qc.clear();
  useComposeStore.getState().setOwner(res.user.id);
  setToken(res.token);
  setCachedMe(qc, res.user);
}

/**
 * Keeps the compose store's owner in step with `['me']`: whenever `['me']` receives data (login,
 * reload, a login in another tab followed by the refetch), compose windows that belong to a
 * different user are closed. Install once at startup (main.tsx). Returns an uninstall function.
 */
export function installComposeOwnerSync(qc: QueryClient): () => void {
  const meHash = hashKey(queryKeys.me());
  return qc.getQueryCache().subscribe((e) => {
    if (e.type !== 'updated' || e.action.type !== 'success') return;
    if (e.query.queryHash !== meHash) return;
    const me = e.query.state.data as Me | undefined;
    if (me) useComposeStore.getState().setOwner(me.id);
  });
}

/**
 * Local half of logout (call after / regardless of POST /api/auth/logout): clears the token and
 * every cached query, then notifies listeners so the router goes to /login.
 */
export function logoutLocally(qc: QueryClient, reason: UnauthorizedReason = 'logout'): void {
  qc.clear();
  emitUnauthorized(reason);
}

// ───────────── multi-tab sync ─────────────

/**
 * Mirrors token changes made by other tabs. Removal → `onUnauthorized('other_tab')`;
 * replacement → `onExternalLogin(token)`. Returns an uninstall function.
 */
export function installAuthStorageSync(target: Window = window): () => void {
  const onStorage = (e: StorageEvent) => {
    // key === null means localStorage.clear() in another tab.
    if (e.key !== null && e.key !== TOKEN_KEY) return;
    const next = e.key === null ? null : e.newValue;
    const current = getToken();
    if (next === current) return;
    if (next === null) {
      useAuthStore.setState({ token: null });
      emitUnauthorized('other_tab');
    } else {
      useAuthStore.setState({ token: next });
      for (const l of [...externalLoginListeners]) l(next);
    }
  };
  target.addEventListener('storage', onStorage);
  return () => target.removeEventListener('storage', onStorage);
}
