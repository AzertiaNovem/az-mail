import { QueryClient } from '@tanstack/react-query';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { queryKeys } from '@/api/queryKeys';
import type { ThreadDetail, WsServerEvent } from '@/api/types';
import { detail, message } from '@/components/mail/testFixtures';
import { COALESCE_MS, createInvalidator, eventEffects, patchOutboundStatus } from './invalidate';

const outbound = { id: 9, status: 'sending' as const, status_detail: null, scheduled_at: null, scheduled_via: null, undo_until: null, sent_at: null };

describe('eventEffects (DESIGN §5 table)', () => {
  it('maps every server event', () => {
    expect(eventEffects({ type: 'ready', user_id: 1, server_time: 0 })).toEqual({ threads: true, counts: true, allThreads: true });
    expect(
      eventEffects({ type: 'mail.new', thread_id: 4, message_id: 8, from: { name: '', email: 'a@b' }, subject: '', snippet: '', in_inbox: true, is_spam: false }),
    ).toEqual({ threads: true, counts: true, threadIds: [4] });
    expect(eventEffects({ type: 'threads.changed', thread_ids: [1, 2] })).toEqual({ threads: true, counts: true, threadIds: [1, 2] });
    expect(eventEffects({ type: 'outbound.status', message_id: 5, thread_id: 3, outbound_id: 9, status: 'sent', status_detail: null })).toEqual({
      threads: true,
      counts: true,
      eventMessageIds: [5],
    });
    expect(eventEffects({ type: 'labels.changed' })).toEqual({ labels: true, counts: true });
    expect(eventEffects({ type: 'settings.changed' })).toEqual({ me: true, threads: true });
    expect(eventEffects({ type: 'pong' })).toEqual({});
    expect(eventEffects({ type: 'session.revoked' })).toEqual({});
    expect(eventEffects({ type: 'threads.changed' } as unknown as WsServerEvent)).toEqual({ threads: true, counts: true, threadIds: [] });
  });

  it('patches the outbound status of one message', () => {
    const d = detail(3, [message(3, { id: 5, direction: 'out', outbound }), message(3, { id: 6 })]);
    const patched = patchOutboundStatus(d, { type: 'outbound.status', message_id: 5, thread_id: 3, outbound_id: 9, status: 'bounced', status_detail: '550' })!;
    expect(patched.messages[0]!.outbound).toMatchObject({ status: 'bounced', status_detail: '550', id: 9 });
    expect(patched.messages[1]).toBe(d.messages[1]);
    expect(patchOutboundStatus(d, { type: 'outbound.status', message_id: 6, thread_id: 3, outbound_id: 9, status: 'sent', status_detail: null })).toBeNull();
  });
});

describe('createInvalidator', () => {
  let qc: QueryClient;
  let spy: ReturnType<typeof vi.spyOn>;
  beforeEach(() => {
    vi.useFakeTimers();
    qc = new QueryClient();
    spy = vi.spyOn(qc, 'invalidateQueries');
  });
  afterEach(() => {
    vi.useRealTimers();
    qc.clear();
  });
  const keys = () => spy.mock.calls.map((c: unknown[]) => JSON.stringify((c[0] as { queryKey: unknown }).queryKey));

  it(`coalesces a burst into one batch after ${COALESCE_MS} ms`, () => {
    const inv = createInvalidator(qc);
    for (let i = 0; i < 20; i++) inv.handle({ type: 'threads.changed', thread_ids: [i % 3] });
    inv.handle({ type: 'labels.changed' });
    expect(spy).not.toHaveBeenCalled();
    vi.advanceTimersByTime(COALESCE_MS - 1);
    expect(spy).not.toHaveBeenCalled();
    vi.advanceTimersByTime(1);
    expect(keys().sort()).toEqual(['["counts"]', '["labels"]', '["thread",0]', '["thread",1]', '["thread",2]', '["threads"]'].sort());
    // The window closes after the batch: a later event starts a new one.
    spy.mockClear();
    inv.handle({ type: 'settings.changed' });
    vi.advanceTimersByTime(COALESCE_MS);
    expect(keys().sort()).toEqual(['["me"]', '["threads"]']);
  });

  it('ready invalidates every thread detail instead of single ids', () => {
    const inv = createInvalidator(qc);
    inv.handle({ type: 'mail.new', thread_id: 7, message_id: 1, from: { name: '', email: 'x@y' }, subject: '', snippet: '', in_inbox: true, is_spam: false });
    inv.handle({ type: 'ready', user_id: 1, server_time: 0 });
    inv.flush();
    expect(keys().sort()).toEqual(['["counts"]', '["thread"]', '["threads"]']);
  });

  it('patches a cached thread in place on outbound.status and invalidates its events', () => {
    const d = detail(3, [message(3, { id: 5, direction: 'out', outbound })]);
    qc.setQueryData(queryKeys.thread(3), d);
    const inv = createInvalidator(qc);
    inv.handle({ type: 'outbound.status', message_id: 5, thread_id: 3, outbound_id: 9, status: 'delivered', status_detail: null });
    expect(qc.getQueryData<ThreadDetail>(queryKeys.thread(3))!.messages[0]!.outbound!.status).toBe('delivered');
    inv.flush();
    expect(keys().sort()).toEqual(['["counts"]', '["message",5,"events"]', '["threads"]']);
  });

  it('invalidates the thread when the message is not cached', () => {
    const inv = createInvalidator(qc);
    inv.handle({ type: 'outbound.status', message_id: 5, thread_id: 3, outbound_id: 9, status: 'sent', status_detail: null });
    inv.flush();
    expect(keys()).toContain('["thread",3]');
  });

  it('ignores pongs and malformed frames; dispose drops the pending batch', () => {
    const inv = createInvalidator(qc);
    inv.handle({ type: 'pong' });
    inv.handle(null as unknown as WsServerEvent);
    vi.advanceTimersByTime(1000);
    expect(spy).not.toHaveBeenCalled();
    inv.handle({ type: 'labels.changed' });
    inv.dispose();
    vi.advanceTimersByTime(1000);
    expect(spy).not.toHaveBeenCalled();
  });

  it('never touches draft caches', () => {
    qc.setQueryData(queryKeys.draft(1), { id: 1 });
    const inv = createInvalidator(qc);
    inv.handle({ type: 'ready', user_id: 1, server_time: 0 });
    inv.handle({ type: 'threads.changed', thread_ids: [1] });
    inv.flush();
    expect(keys().some((k: string) => k.startsWith('["draft"'))).toBe(false);
    expect(qc.getQueryState(queryKeys.draft(1))!.isInvalidated).toBe(false);
  });
});
