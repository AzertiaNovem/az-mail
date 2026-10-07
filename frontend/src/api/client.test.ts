import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { clearToken, getToken, onUnauthorized, setToken, TOKEN_KEY } from '@/stores/auth';
import { ApiError, buildQuery, endsSession, isApiError, parseErrorResponse, request, uploadRaw } from './client';
import {
  changePassword,
  listThreads,
  listThreadsFor,
  login,
  MAX_ATTACHMENT_BYTES,
  MAX_MESSAGE_ATTACHMENTS_BYTES,
  uploadAttachment,
} from './endpoints';

type FetchArgs = [input: string, init: RequestInit];

function jsonResponse(status: number, body: unknown): Response {
  return new Response(body === undefined ? null : JSON.stringify(body), {
    status,
    headers: { 'Content-Type': 'application/json' },
  });
}

function mockFetch(impl: (...args: FetchArgs) => Promise<Response> | Response) {
  const fn = vi.fn(impl);
  vi.stubGlobal('fetch', fn);
  return fn;
}

beforeEach(() => clearToken());

describe('buildQuery', () => {
  it('percent-encodes, skips empty values and maps booleans to 1/0', () => {
    expect(buildQuery({ filename: '季度 报告+v2.pdf', inline: true, x: undefined, y: null, z: '' })).toBe(
      '?filename=%E5%AD%A3%E5%BA%A6%20%E6%8A%A5%E5%91%8A%2Bv2.pdf&inline=1',
    );
    expect(buildQuery({ inline: false, n: 0 })).toBe('?inline=0&n=0');
    expect(buildQuery({})).toBe('');
    expect(buildQuery(undefined)).toBe('');
  });
});

describe('parseErrorResponse', () => {
  it('parses the {"error":{code,message,details}} envelope', () => {
    const e = parseErrorResponse(
      409,
      JSON.stringify({ error: { code: 'version_conflict', message: '草稿已更改', details: { current: { id: 1 } } } }),
    );
    expect(e).toBeInstanceOf(ApiError);
    expect(e.status).toBe(409);
    expect(e.code).toBe('version_conflict');
    expect(e.message).toBe('草稿已更改');
    expect(e.details).toEqual({ current: { id: 1 } });
  });

  it('falls back to a localized message when the server sends none', () => {
    const e = parseErrorResponse(422, JSON.stringify({ error: { code: 'unknown_local_recipient', message: '' } }));
    expect(e.message).toBe('收件人地址不存在');
    expect(e.details).toEqual({});
  });

  it('handles non-JSON bodies (proxies, HTML error pages)', () => {
    const e = parseErrorResponse(502, '<html>Bad Gateway</html>');
    expect(e.code).toBe('http_502');
    expect(e.status).toBe(502);
    expect(e.message).toBe('服务器出错，请稍后重试');
    expect(parseErrorResponse(404, '').code).toBe('http_404');
    expect(parseErrorResponse(503, '').message).toBe('服务繁忙，请稍后再试');
  });

  it('endsSession: every 401 except a failed credential check', () => {
    expect(endsSession(new ApiError(401, 'unauthorized', 'x'))).toBe(true);
    expect(endsSession(new ApiError(401, 'http_401', 'x'))).toBe(true);
    expect(endsSession(new ApiError(401, 'invalid_credentials', 'x'))).toBe(false);
    expect(endsSession(new ApiError(403, 'invalid_credentials', 'x'))).toBe(false);
    expect(endsSession(new ApiError(403, 'forbidden', 'x'))).toBe(false);
  });
});

describe('request', () => {
  it('sends JSON with the bearer token and decodes the response', async () => {
    setToken('tok-1');
    const fetchFn = mockFetch(() => jsonResponse(200, { ok: true }));
    const res = await request<{ ok: boolean }>('/api/x', { method: 'POST', body: { a: 1 }, query: { q: 'a b' } });
    expect(res).toEqual({ ok: true });
    const [url, init] = fetchFn.mock.calls[0]!;
    expect(url).toBe('/api/x?q=a%20b');
    expect(init.method).toBe('POST');
    expect(init.body).toBe('{"a":1}');
    const headers = init.headers as Record<string, string>;
    expect(headers.Authorization).toBe('Bearer tok-1');
    expect(headers['Content-Type']).toBe('application/json');
  });

  it('resolves 204 and empty bodies to undefined', async () => {
    mockFetch(() => new Response(null, { status: 204 }));
    await expect(request('/api/auth/logout', { method: 'POST' })).resolves.toBeUndefined();
    mockFetch(() => new Response('', { status: 200 }));
    await expect(request('/api/x')).resolves.toBeUndefined();
  });

  it('rejects with a parsed ApiError on HTTP errors', async () => {
    setToken('tok');
    mockFetch(() => jsonResponse(403, { error: { code: 'send_as_forbidden', message: '无权以此地址发送', details: {} } }));
    const err = await request('/api/drafts/1/send', { method: 'POST' }).catch((e: unknown) => e);
    expect(isApiError(err, 'send_as_forbidden')).toBe(true);
    expect((err as ApiError).status).toBe(403);
    expect(getToken()).toBe('tok'); // only 401 ends the session
  });

  it('maps network failures to network_error', async () => {
    mockFetch(() => Promise.reject(new TypeError('Failed to fetch')));
    const err = await request('/api/x').catch((e: unknown) => e);
    expect(isApiError(err, 'network_error')).toBe(true);
    expect((err as ApiError).status).toBe(0);
  });

  it('propagates aborts as AbortError, not ApiError', async () => {
    const ctrl = new AbortController();
    mockFetch((_url, init) => {
      return new Promise<Response>((_resolve, reject) => {
        init.signal?.addEventListener('abort', () => reject(new DOMException('Aborted', 'AbortError')));
      });
    });
    const p = request('/api/x', { signal: ctrl.signal });
    ctrl.abort();
    const err = await p.catch((e: unknown) => e);
    expect(err).not.toBeInstanceOf(ApiError);
    expect((err as Error).name).toBe('AbortError');
  });

  it('on 401 clears the token and emits unauthorized', async () => {
    setToken('stale');
    const seen: string[] = [];
    const off = onUnauthorized((r) => seen.push(r));
    mockFetch(() => jsonResponse(401, { error: { code: 'unauthorized', message: '请先登录' } }));
    const err = await request('/api/auth/me').catch((e: unknown) => e);
    off();
    expect((err as ApiError).status).toBe(401);
    expect(getToken()).toBeNull();
    expect(localStorage.getItem(TOKEN_KEY)).toBeNull();
    expect(seen).toEqual(['expired']);
  });

  it('ignores a 401 for a token that has since been replaced', async () => {
    setToken('old');
    const seen: string[] = [];
    const off = onUnauthorized((r) => seen.push(r));
    mockFetch(async () => {
      setToken('new'); // a fresh login lands while the old request is in flight
      return jsonResponse(401, { error: { code: 'unauthorized', message: 'x' } });
    });
    await request('/api/x').catch(() => undefined);
    off();
    expect(getToken()).toBe('new');
    expect(seen).toEqual([]);
  });

  it('does not treat a failed login (no token sent) as a session end', async () => {
    const seen: string[] = [];
    const off = onUnauthorized((r) => seen.push(r));
    const fetchFn = mockFetch(() => jsonResponse(401, { error: { code: 'invalid_credentials', message: '邮箱或密码错误' } }));
    const err = await login({ email: 'a@b.c', password: 'x' }).catch((e: unknown) => e);
    off();
    expect(isApiError(err, 'invalid_credentials')).toBe(true);
    expect((fetchFn.mock.calls[0]![1].headers as Record<string, string>).Authorization).toBeUndefined();
    expect(seen).toEqual([]);
  });

  it('a wrong current password (401 or 403 invalid_credentials) keeps the session', async () => {
    setToken('live');
    const seen: string[] = [];
    const off = onUnauthorized((r) => seen.push(r));
    for (const status of [401, 403]) {
      mockFetch(() => jsonResponse(status, { error: { code: 'invalid_credentials', message: '当前密码错误' } }));
      const err = await changePassword({ current_password: 'typo', new_password: 'n3w-pass' }).catch((e: unknown) => e);
      expect(isApiError(err, 'invalid_credentials')).toBe(true);
    }
    off();
    expect(getToken()).toBe('live');
    expect(seen).toEqual([]);
  });

  it('listThreads sends tzoff and encodes the search query', async () => {
    const fetchFn = mockFetch(() => jsonResponse(200, { items: [], next_cursor: null, total: null }));
    await listThreads({ q: 'from:张三 "周报"', limit: 50 });
    const url = new URL(fetchFn.mock.calls[0]![0], 'http://x');
    expect(url.searchParams.get('q')).toBe('from:张三 "周报"');
    expect(url.searchParams.get('limit')).toBe('50');
    expect(url.searchParams.get('tzoff')).toBe(String(-new Date().getTimezoneOffset()));
    expect(url.searchParams.has('folder')).toBe(false);
  });

  it('listThreadsFor sends exactly one of folder / label_id / q, as the cache key does', async () => {
    const fetchFn = mockFetch(() => jsonResponse(200, { items: [], next_cursor: null, total: 0 }));
    await listThreadsFor({ folder: 'sent', q: ' from:bob ' }, 'c2', 25);
    await listThreadsFor({ folder: 'sent', labelId: 4 }, null, 50);
    await listThreadsFor({}, null, 50);
    const params = fetchFn.mock.calls.map(([u]) => new URL(u, 'http://x').searchParams);
    expect([params[0]!.get('q'), params[0]!.has('folder'), params[0]!.get('cursor'), params[0]!.get('limit')]).toEqual([
      'from:bob',
      false,
      'c2',
      '25',
    ]);
    expect([params[1]!.get('label_id'), params[1]!.has('folder'), params[1]!.has('cursor')]).toEqual(['4', false, false]);
    expect([params[2]!.get('folder'), params[2]!.has('label_id'), params[2]!.has('q')]).toEqual(['inbox', false, false]);
  });
});

// ───────────── XHR upload ─────────────

class FakeXHR {
  static last: FakeXHR | null = null;
  static respond: { status: number; body: string } = { status: 201, body: '{}' };
  method = '';
  url = '';
  headers: Record<string, string> = {};
  body: unknown;
  status = 0;
  responseText = '';
  upload: { onprogress: ((e: { loaded: number; total: number; lengthComputable: boolean }) => void) | null } = {
    onprogress: null,
  };
  onload: (() => void) | null = null;
  onerror: (() => void) | null = null;
  ontimeout: (() => void) | null = null;
  onabort: (() => void) | null = null;
  constructor() {
    FakeXHR.last = this;
  }
  open(method: string, url: string) {
    this.method = method;
    this.url = url;
  }
  setRequestHeader(k: string, v: string) {
    this.headers[k] = v;
  }
  send(body: unknown) {
    this.body = body;
    queueMicrotask(() => {
      this.upload.onprogress?.({ loaded: 5, total: 10, lengthComputable: true });
      this.upload.onprogress?.({ loaded: 10, total: 10, lengthComputable: true });
      this.status = FakeXHR.respond.status;
      this.responseText = FakeXHR.respond.body;
      this.onload?.();
    });
  }
  abort() {
    this.onabort?.();
  }
}

describe('uploadRaw / uploadAttachment', () => {
  afterEach(() => {
    FakeXHR.last = null;
  });

  it('posts the raw body with progress and decodes the Attachment', async () => {
    vi.stubGlobal('XMLHttpRequest', FakeXHR);
    setToken('tok');
    const att = { id: 9, filename: '报告.pdf', content_type: 'application/pdf', size: 10 };
    FakeXHR.respond = { status: 201, body: JSON.stringify(att) };
    const progress: number[] = [];
    const file = new File(['0123456789'], '报告.pdf', { type: 'application/pdf' });
    const res = await uploadAttachment(file, { onProgress: (p) => progress.push(p.fraction) });
    const xhr = FakeXHR.last!;
    expect(res).toEqual(att);
    expect(xhr.method).toBe('POST');
    expect(xhr.url).toBe('/api/attachments?filename=%E6%8A%A5%E5%91%8A.pdf&inline=0');
    expect(xhr.headers['Content-Type']).toBe('application/pdf');
    expect(xhr.headers.Authorization).toBe('Bearer tok');
    expect(xhr.body).toBe(file);
    expect(progress).toEqual([0.5, 1]);
  });

  it('rejects with ApiError on 413 and ends the session on 401', async () => {
    vi.stubGlobal('XMLHttpRequest', FakeXHR);
    setToken('tok');
    FakeXHR.respond = {
      status: 413,
      body: JSON.stringify({ error: { code: 'payload_too_large', message: '文件过大', details: { limit: 26214400 } } }),
    };
    const err = await uploadRaw('/api/attachments', new Blob(['x'])).catch((e: unknown) => e);
    expect(isApiError(err, 'payload_too_large')).toBe(true);
    expect((err as ApiError).details).toEqual({ limit: 26214400 });
    expect(getToken()).toBe('tok');

    FakeXHR.respond = { status: 401, body: '' };
    const err2 = await uploadRaw('/api/attachments', new Blob(['x'])).catch((e: unknown) => e);
    expect((err2 as ApiError).status).toBe(401);
    expect(getToken()).toBeNull();
  });

  it('rejects files over 25 MiB locally with 413 payload_too_large (no request sent)', async () => {
    vi.stubGlobal('XMLHttpRequest', FakeXHR);
    expect(MAX_ATTACHMENT_BYTES).toBe(25 * 1024 * 1024);
    expect(MAX_MESSAGE_ATTACHMENTS_BYTES).toBe(28 * 1024 * 1024);
    const big = new File(['x'], '视频.mp4', { type: 'video/mp4' });
    Object.defineProperty(big, 'size', { value: MAX_ATTACHMENT_BYTES + 1 });
    const err = await uploadAttachment(big).catch((e: unknown) => e);
    expect(isApiError(err, 'payload_too_large')).toBe(true);
    expect((err as ApiError).status).toBe(413);
    expect((err as ApiError).message).toBe('文件过大（单个附件最大 25 MB）');
    expect(FakeXHR.last).toBeNull();
  });

  it('a 401 invalid_credentials upload response keeps the session', async () => {
    vi.stubGlobal('XMLHttpRequest', FakeXHR);
    setToken('tok');
    FakeXHR.respond = { status: 401, body: JSON.stringify({ error: { code: 'invalid_credentials', message: 'x' } }) };
    await uploadRaw('/api/attachments', new Blob(['x'])).catch(() => undefined);
    expect(getToken()).toBe('tok');
  });

  it('aborts via AbortSignal', async () => {
    vi.stubGlobal('XMLHttpRequest', FakeXHR);
    const ctrl = new AbortController();
    ctrl.abort();
    const err = await uploadRaw('/api/attachments', new Blob(['x']), { signal: ctrl.signal }).catch((e: unknown) => e);
    expect((err as Error).name).toBe('AbortError');
  });
});
