import { describe, expect, it, vi } from 'vitest';
import { clearToken, setToken } from '@/stores/auth';
import { adminKeys, adminListEvents, adminRetryJob, adminRetryOutbox, adminSync } from './admin';
import { staleTimes } from './queryKeys';

function mockFetch(body: unknown, status = 200) {
  const fn = vi.fn(
    async (_url: string, _init: RequestInit) =>
      new Response(JSON.stringify(body), { status, headers: { 'Content-Type': 'application/json' } }),
  );
  vi.stubGlobal('fetch', fn);
  return fn;
}

describe('admin api', () => {
  it('keys all share the ["admin"] prefix (prefix invalidation, staleTime 0)', () => {
    const keys = [
      adminKeys.users(),
      adminKeys.aliases(),
      adminKeys.domains(),
      adminKeys.domain(3),
      adminKeys.domainStatus(3),
      adminKeys.events('email.bounced', 'c1'),
      adminKeys.inbound('failed'),
      adminKeys.outbox('failed'),
      adminKeys.jobs('dead'),
      adminKeys.stats(),
    ];
    for (const k of keys) expect(k.slice(0, 1)).toEqual(adminKeys.all());
    // Per-domain keys live under the domain-list prefix, so invalidating domains refreshes them too.
    expect(adminKeys.domain(3).slice(0, 2)).toEqual(adminKeys.domains());
    expect(adminKeys.domainStatus(3).slice(0, 3)).toEqual(adminKeys.domain(3));
    expect(adminKeys.events('')).toEqual(['admin', 'events', { type: null }, null]);
    expect(staleTimes.admin).toBe(0);
  });

  it('lists events with type + cursor and decodes the cursor page', async () => {
    setToken('adm');
    const page = { items: [], next_cursor: 'n2' };
    const fetchFn = mockFetch(page);
    await expect(adminListEvents({ type: 'email.bounced', cursor: 'n1' })).resolves.toEqual(page);
    const [url, init] = fetchFn.mock.calls[0]!;
    expect(url).toBe('/api/admin/events?type=email.bounced&cursor=n1');
    expect((init.headers as Record<string, string>).Authorization).toBe('Bearer adm');
    clearToken();
  });

  it('posts sync (202 {job_id}) and job retry', async () => {
    const fetchFn = mockFetch({ job_id: 77 }, 202);
    await expect(adminSync()).resolves.toEqual({ job_id: 77 });
    await adminRetryJob(5);
    expect(fetchFn.mock.calls.map(([u, i]) => `${i.method} ${u}`)).toEqual([
      'POST /api/admin/sync',
      'POST /api/admin/jobs/5/retry',
    ]);
  });

  it('outbox retry returns the NEW row; every outbox filter shares the ["admin","outbox"] prefix', async () => {
    const fresh = { id: 12, uuid: 'new-uuid', status: 'queued' };
    const fetchFn = mockFetch(fresh);
    await expect(adminRetryOutbox(9)).resolves.toEqual(fresh);
    const [url, init] = fetchFn.mock.calls[0]!;
    expect(`${init.method} ${url}`).toBe('POST /api/admin/outbox/9/retry');
    for (const k of [adminKeys.outbox(), adminKeys.outbox('failed'), adminKeys.outbox('queued')])
      expect(k.slice(0, 2)).toEqual(['admin', 'outbox']);
  });
});
