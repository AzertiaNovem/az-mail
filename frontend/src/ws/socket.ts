/**
 * Realtime socket [WP-E] (docs/API.md "WebSocket protocol", DESIGN.md §1 A4 / §5).
 *
 * - Connects to `wsUrl()` (`/api/ws` on the API origin) and authenticates with the first
 *   message `{"type":"auth","token":…}`; the connection counts as open on `ready`.
 * - Reconnects with exponential backoff 1 s → 2 → 4 → 8 → 16 → 30 s (±20 % jitter, capped),
 *   reset after a `ready`; immediately when the browser comes back online or the tab becomes
 *   visible again.
 * - Sends `{"type":"ping"}` every 25 s; a socket silent for 35 s is considered dead and
 *   replaced. A connection without `ready` 10 s after opening is retried too.
 * - `session.revoked` → `emitUnauthorized('revoked')` (logout everywhere, no reconnect).
 *   Close 4401 without it (auth rejected / token expired) → `onAuthRejected` (the app re-checks
 *   the session with GET /api/auth/me: a 401 there logs out) and a backoff retry.
 * - Follows the auth store: no token → disconnected; a new token → reconnect with it.
 * - `useSocketStore.status` drives the `['counts']` polling fallback (60 s while not open).
 */
import type { QueryClient } from '@tanstack/react-query';
import { create } from 'zustand';
import { getMe } from '@/api/endpoints';
import { WS_CLOSE_AUTH_FAILED, type WsServerEvent } from '@/api/types';
import { wsUrl } from '@/config';
import { emitUnauthorized, getToken, setCachedMe, useAuthStore } from '@/stores/auth';
import { createInvalidator } from './invalidate';

export type SocketStatus = 'idle' | 'connecting' | 'open' | 'closed';

interface SocketState {
  status: SocketStatus;
  /** Consecutive failed attempts since the last `ready`. */
  attempt: number;
  /** When the next reconnect is scheduled (ms epoch), if any. */
  retryAt: number | null;
}

export const useSocketStore = create<SocketState>(() => ({ status: 'idle', attempt: 0, retryAt: null }));

/** React hook: whether realtime updates are flowing. */
export const useSocketOpen = (): boolean => useSocketStore((s) => s.status === 'open');

export const BACKOFF_BASE_MS = 1000;
export const BACKOFF_MAX_MS = 30_000;
export const PING_INTERVAL_MS = 25_000;
export const PONG_TIMEOUT_MS = 10_000;
export const READY_TIMEOUT_MS = 10_000;
/** Close code we use for a dead (silent) socket. */
export const CLOSE_STALE = 4000;

/** Delay before reconnect attempt `attempt` (0-based): 1, 2, 4, 8, 16, 30, 30 … s, ±20 %, capped. */
export function backoffDelay(attempt: number, random: () => number = Math.random): number {
  const base = Math.min(BACKOFF_MAX_MS, BACKOFF_BASE_MS * 2 ** Math.max(0, Math.min(attempt, 10)));
  const jittered = Math.round(base * (0.8 + 0.4 * random()));
  return Math.max(BACKOFF_BASE_MS * 0.8, Math.min(BACKOFF_MAX_MS, jittered));
}

/** Parses a server frame; null for anything that is not a typed JSON object. */
export function parseServerFrame(data: unknown): WsServerEvent | null {
  if (typeof data !== 'string') return null;
  try {
    const v: unknown = JSON.parse(data);
    if (typeof v === 'object' && v !== null && typeof (v as { type?: unknown }).type === 'string') return v as WsServerEvent;
  } catch {
    /* not JSON */
  }
  return null;
}

type WebSocketCtor = new (url: string) => WebSocket;

export interface RealtimeDeps {
  url: () => string;
  getToken: () => string | null;
  onEvent: (e: WsServerEvent) => void;
  /** `session.revoked` frame. */
  onRevoked: () => void;
  /** Close 4401 without a revoke frame. */
  onAuthRejected: () => void;
  WebSocketImpl?: WebSocketCtor;
  random?: () => number;
  now?: () => number;
}

const OPEN = 1;

export class RealtimeClient {
  private ws: WebSocket | null = null;
  private running = false;
  private attempt = 0;
  private retryTimer: ReturnType<typeof setTimeout> | null = null;
  private pingTimer: ReturnType<typeof setInterval> | null = null;
  private readyTimer: ReturnType<typeof setTimeout> | null = null;
  private lastMessageAt = 0;
  private revoked = false;
  private readonly deps: RealtimeDeps;

  constructor(deps: RealtimeDeps) {
    this.deps = deps;
  }

  private now(): number {
    return (this.deps.now ?? Date.now)();
  }

  private setStatus(status: SocketStatus, retryAt: number | null = null): void {
    useSocketStore.setState({ status, attempt: this.attempt, retryAt });
  }

  /** Starts (or keeps) the connection. Idempotent. */
  start(): void {
    if (this.running) return;
    this.running = true;
    this.revoked = false;
    this.attempt = 0;
    this.connect();
  }

  /** Closes the socket and stops reconnecting. */
  stop(): void {
    this.running = false;
    this.clearTimers();
    this.closeSocket(1000, 'stop');
    this.setStatus('idle');
  }

  /** Drops the current connection and connects again now (new token). */
  restart(): void {
    this.stop();
    this.start();
  }

  /** Skips the backoff wait when the socket is down (online / visible again). */
  reconnectNow(): void {
    if (!this.running || this.revoked) return;
    if (this.ws && this.ws.readyState <= OPEN) return;
    if (this.retryTimer !== null) clearTimeout(this.retryTimer);
    this.retryTimer = null;
    this.connect();
  }

  private clearTimers(): void {
    if (this.retryTimer !== null) clearTimeout(this.retryTimer);
    if (this.pingTimer !== null) clearInterval(this.pingTimer);
    if (this.readyTimer !== null) clearTimeout(this.readyTimer);
    this.retryTimer = null;
    this.pingTimer = null;
    this.readyTimer = null;
  }

  private closeSocket(code: number, reason: string): void {
    const ws = this.ws;
    this.ws = null;
    if (!ws) return;
    ws.onopen = ws.onmessage = ws.onerror = ws.onclose = null;
    try {
      ws.close(code, reason);
    } catch {
      /* already closed */
    }
  }

  private connect(): void {
    if (!this.running) return;
    const token = this.deps.getToken();
    if (!token) {
      this.setStatus('idle');
      return;
    }
    const Impl = this.deps.WebSocketImpl ?? (WebSocket as unknown as WebSocketCtor);
    let ws: WebSocket;
    try {
      ws = new Impl(this.deps.url());
    } catch {
      this.scheduleReconnect();
      return;
    }
    this.ws = ws;
    this.setStatus('connecting');

    ws.onopen = () => {
      if (ws !== this.ws) return;
      const current = this.deps.getToken();
      if (!current) {
        this.stop();
        return;
      }
      ws.send(JSON.stringify({ type: 'auth', token: current }));
      this.lastMessageAt = this.now();
      this.readyTimer = setTimeout(() => {
        if (ws === this.ws && useSocketStore.getState().status !== 'open') ws.close(CLOSE_STALE, 'no ready');
      }, READY_TIMEOUT_MS);
    };

    ws.onmessage = (ev: MessageEvent) => {
      if (ws !== this.ws) return;
      this.lastMessageAt = this.now();
      const frame = parseServerFrame(ev.data);
      if (!frame) return;
      if (frame.type === 'ready') {
        this.attempt = 0;
        if (this.readyTimer !== null) clearTimeout(this.readyTimer);
        this.readyTimer = null;
        this.startPing(ws);
        this.setStatus('open');
      } else if (frame.type === 'session.revoked') {
        this.revoked = true;
      }
      try {
        this.deps.onEvent(frame);
      } catch (e) {
        console.error('realtime event handler failed', e);
      }
      if (frame.type === 'session.revoked') {
        this.stop();
        this.deps.onRevoked();
      }
    };

    ws.onerror = () => {
      /* a close event follows */
    };

    ws.onclose = (ev: CloseEvent) => {
      if (ws !== this.ws) return;
      this.ws = null;
      this.clearTimers();
      if (!this.running) return;
      if (ev.code === WS_CLOSE_AUTH_FAILED) {
        if (this.revoked) {
          this.stop();
          return;
        }
        this.deps.onAuthRejected();
      }
      this.scheduleReconnect();
    };
  }

  private startPing(ws: WebSocket): void {
    if (this.pingTimer !== null) clearInterval(this.pingTimer);
    this.pingTimer = setInterval(() => {
      if (ws !== this.ws || ws.readyState !== OPEN) return;
      if (this.now() - this.lastMessageAt > PING_INTERVAL_MS + PONG_TIMEOUT_MS) {
        ws.close(CLOSE_STALE, 'stale');
        return;
      }
      try {
        ws.send(JSON.stringify({ type: 'ping' }));
      } catch {
        /* the close event reconnects */
      }
    }, PING_INTERVAL_MS);
  }

  private scheduleReconnect(): void {
    if (!this.running || this.revoked) return;
    const delay = backoffDelay(this.attempt, this.deps.random);
    this.attempt++;
    if (this.retryTimer !== null) clearTimeout(this.retryTimer);
    this.retryTimer = setTimeout(() => {
      this.retryTimer = null;
      this.connect();
    }, delay);
    this.setStatus('closed', this.now() + delay);
  }
}

/**
 * Wires the realtime socket to the app: cache invalidation, auth store, online / visibility
 * events. Returns the teardown function (AppShell mounts it once per authenticated session).
 */
export function startRealtime(qc: QueryClient): () => void {
  const invalidator = createInvalidator(qc);
  const client = new RealtimeClient({
    url: () => wsUrl(),
    getToken,
    onEvent: (e) => invalidator.handle(e),
    onRevoked: () => emitUnauthorized('revoked'),
    onAuthRejected: () => {
      // A 401 here ends the session through the API client; success means a transient failure.
      getMe()
        .then((me) => setCachedMe(qc, me))
        .catch(() => {});
    },
  });
  client.start();

  const unsubscribe = useAuthStore.subscribe((s, prev) => {
    if (s.token === prev.token) return;
    if (s.token) client.restart();
    else client.stop();
  });
  const onOnline = () => client.reconnectNow();
  const onVisible = () => {
    if (document.visibilityState === 'visible') client.reconnectNow();
  };
  window.addEventListener('online', onOnline);
  document.addEventListener('visibilitychange', onVisible);

  return () => {
    unsubscribe();
    window.removeEventListener('online', onOnline);
    document.removeEventListener('visibilitychange', onVisible);
    client.stop();
    invalidator.dispose();
  };
}
