import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import type { WsServerEvent } from '@/api/types';
import {
  BACKOFF_MAX_MS,
  backoffDelay,
  parseServerFrame,
  PING_INTERVAL_MS,
  PONG_TIMEOUT_MS,
  READY_TIMEOUT_MS,
  RealtimeClient,
  useSocketStore,
} from './socket';

class FakeSocket {
  static instances: FakeSocket[] = [];
  readyState = 0;
  sent: string[] = [];
  closed: { code?: number; reason?: string } | null = null;
  onopen: ((e: Event) => void) | null = null;
  onmessage: ((e: MessageEvent) => void) | null = null;
  onerror: ((e: Event) => void) | null = null;
  onclose: ((e: CloseEvent) => void) | null = null;
  url: string;
  constructor(url: string) {
    this.url = url;
    FakeSocket.instances.push(this);
  }
  send(data: string) {
    this.sent.push(data);
  }
  close(code?: number, reason?: string) {
    this.closed = { code, reason };
    this.readyState = 3;
    this.onclose?.({ code: code ?? 1005 } as CloseEvent);
  }
  // test helpers
  open() {
    this.readyState = 1;
    this.onopen?.(new Event('open'));
  }
  receive(frame: unknown) {
    this.onmessage?.({ data: typeof frame === 'string' ? frame : JSON.stringify(frame) } as MessageEvent);
  }
  serverClose(code: number) {
    this.readyState = 3;
    this.onclose?.({ code } as CloseEvent);
  }
}

const last = () => FakeSocket.instances[FakeSocket.instances.length - 1]!;

function setup(token: string | null = 'tok') {
  const events: WsServerEvent[] = [];
  const onRevoked = vi.fn();
  const onAuthRejected = vi.fn();
  let currentToken = token;
  const client = new RealtimeClient({
    url: () => 'ws://api.test/api/ws',
    getToken: () => currentToken,
    onEvent: (e) => events.push(e),
    onRevoked,
    onAuthRejected,
    WebSocketImpl: FakeSocket as unknown as new (url: string) => WebSocket,
    random: () => 0.5,
  });
  return { client, events, onRevoked, onAuthRejected, setToken: (t: string | null) => (currentToken = t) };
}

beforeEach(() => {
  vi.useFakeTimers();
  FakeSocket.instances = [];
  useSocketStore.setState({ status: 'idle', attempt: 0, retryAt: null });
});
afterEach(() => vi.useRealTimers());

describe('backoff', () => {
  it('doubles from 1 s up to 30 s', () => {
    const half = () => 0.5; // no jitter
    expect([0, 1, 2, 3, 4, 5, 6, 20].map((a) => backoffDelay(a, half))).toEqual([1000, 2000, 4000, 8000, 16000, 30000, 30000, 30000]);
  });
  it('jitters by ±20 % and never exceeds the cap', () => {
    expect(backoffDelay(0, () => 0)).toBe(800);
    expect(backoffDelay(0, () => 1)).toBe(1200);
    expect(backoffDelay(10, () => 1)).toBe(BACKOFF_MAX_MS);
  });
});

describe('parseServerFrame', () => {
  it('accepts typed JSON objects only', () => {
    expect(parseServerFrame('{"type":"pong"}')).toEqual({ type: 'pong' });
    expect(parseServerFrame('nope')).toBeNull();
    expect(parseServerFrame('{"x":1}')).toBeNull();
    expect(parseServerFrame('[1]')).toBeNull();
    expect(parseServerFrame(new ArrayBuffer(2))).toBeNull();
  });
});

describe('RealtimeClient', () => {
  it('authenticates with the first message and is open on ready', () => {
    const { client, events } = setup();
    client.start();
    expect(last().url).toBe('ws://api.test/api/ws');
    expect(useSocketStore.getState().status).toBe('connecting');
    last().open();
    expect(last().sent).toEqual([JSON.stringify({ type: 'auth', token: 'tok' })]);
    last().receive({ type: 'ready', user_id: 1, server_time: 5 });
    expect(useSocketStore.getState().status).toBe('open');
    last().receive({ type: 'threads.changed', thread_ids: [3] });
    last().receive('garbage');
    expect(events.map((e) => e.type)).toEqual(['ready', 'threads.changed']);
    client.stop();
    expect(last().closed?.code).toBe(1000);
    expect(useSocketStore.getState().status).toBe('idle');
  });

  it('reconnects with exponential backoff and resets after ready', () => {
    const { client } = setup();
    client.start();
    last().serverClose(1006);
    expect(useSocketStore.getState().status).toBe('closed');
    expect(FakeSocket.instances).toHaveLength(1);
    vi.advanceTimersByTime(999);
    expect(FakeSocket.instances).toHaveLength(1);
    vi.advanceTimersByTime(1);
    expect(FakeSocket.instances).toHaveLength(2);
    last().serverClose(1006);
    vi.advanceTimersByTime(1999);
    expect(FakeSocket.instances).toHaveLength(2);
    vi.advanceTimersByTime(1);
    expect(FakeSocket.instances).toHaveLength(3);
    last().open();
    last().receive({ type: 'ready', user_id: 1, server_time: 0 });
    last().serverClose(1006);
    vi.advanceTimersByTime(1000); // back to 1 s
    expect(FakeSocket.instances).toHaveLength(4);
    client.stop();
  });

  it('session.revoked logs out and stops reconnecting', () => {
    const { client, onRevoked } = setup();
    client.start();
    last().open();
    last().receive({ type: 'ready', user_id: 1, server_time: 0 });
    last().receive({ type: 'session.revoked' });
    expect(onRevoked).toHaveBeenCalledOnce();
    vi.advanceTimersByTime(60_000);
    expect(FakeSocket.instances).toHaveLength(1);
  });

  it('close 4401 without a revoke frame re-checks the session and retries', () => {
    const { client, onAuthRejected } = setup();
    client.start();
    last().open();
    last().serverClose(4401);
    expect(onAuthRejected).toHaveBeenCalledOnce();
    vi.advanceTimersByTime(1000);
    expect(FakeSocket.instances).toHaveLength(2);
    client.stop();
  });

  it('pings while open and replaces a silent socket', () => {
    const { client } = setup();
    client.start();
    last().open();
    last().receive({ type: 'ready', user_id: 1, server_time: 0 });
    vi.advanceTimersByTime(PING_INTERVAL_MS);
    expect(last().sent.at(-1)).toBe(JSON.stringify({ type: 'ping' }));
    last().receive({ type: 'pong' });
    vi.advanceTimersByTime(PING_INTERVAL_MS);
    expect(last().closed).toBeNull();
    // Nothing for longer than ping + pong timeout → closed and reconnected.
    vi.advanceTimersByTime(PING_INTERVAL_MS + PONG_TIMEOUT_MS);
    expect(FakeSocket.instances[0]!.closed?.code).toBe(4000);
    vi.advanceTimersByTime(1000);
    expect(FakeSocket.instances).toHaveLength(2);
    client.stop();
  });

  it('retries a connection that never becomes ready', () => {
    const { client } = setup();
    client.start();
    last().open();
    vi.advanceTimersByTime(READY_TIMEOUT_MS);
    expect(FakeSocket.instances[0]!.closed?.code).toBe(4000);
    client.stop();
  });

  it('stays idle without a token and reconnects at once on demand', () => {
    const s = setup(null);
    s.client.start();
    expect(FakeSocket.instances).toHaveLength(0);
    expect(useSocketStore.getState().status).toBe('idle');
    s.client.stop();

    const t = setup();
    t.client.start();
    last().serverClose(1006);
    t.client.reconnectNow();
    expect(FakeSocket.instances).toHaveLength(2);
    t.client.reconnectNow(); // already connecting: no duplicate
    expect(FakeSocket.instances).toHaveLength(2);
    t.client.stop();
  });

  it('restart() reconnects with the new token', () => {
    const { client, setToken } = setup();
    client.start();
    last().open();
    setToken('tok2');
    client.restart();
    expect(FakeSocket.instances[0]!.closed?.code).toBe(1000);
    last().open();
    expect(last().sent[0]).toBe(JSON.stringify({ type: 'auth', token: 'tok2' }));
    client.stop();
  });
});
