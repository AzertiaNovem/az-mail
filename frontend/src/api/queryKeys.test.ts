import { hashKey } from '@tanstack/react-query';
import { describe, expect, it } from 'vitest';
import { queryKeys, staleTimes, threadListFilter } from './queryKeys';

describe('queryKeys', () => {
  it('matches the DESIGN.md §5 key shapes', () => {
    expect(queryKeys.me()).toEqual(['me']);
    expect(queryKeys.labels()).toEqual(['labels']);
    expect(queryKeys.counts()).toEqual(['counts']);
    expect(queryKeys.threads({ folder: 'inbox' }, null)).toEqual([
      'threads',
      { folder: 'inbox', labelId: null, q: null },
      null,
    ]);
    expect(queryKeys.thread(42)).toEqual(['thread', 42]);
    expect(queryKeys.draft(7)).toEqual(['draft', 7]);
    expect(queryKeys.contacts('Ab ')).toEqual(['contacts', 'ab']);
    expect(queryKeys.messageEvents(5)).toEqual(['message', 5, 'events']);
  });

  it('is stable for equivalent filters', () => {
    const a = queryKeys.threads({ folder: 'inbox' });
    const b = queryKeys.threads({ q: undefined, labelId: undefined, folder: 'inbox' }, null);
    const c = queryKeys.threads({ folder: 'inbox', labelId: null, q: '   ' });
    expect(hashKey(a)).toBe(hashKey(b));
    expect(hashKey(a)).toBe(hashKey(c));
    // A fresh call yields an equal (not identical) key.
    expect(queryKeys.threads({ folder: 'inbox' })).toEqual(a);
  });

  it('distinguishes views and cursors', () => {
    const inbox = hashKey(queryKeys.threads({ folder: 'inbox' }));
    expect(hashKey(queryKeys.threads({ folder: 'sent' }))).not.toBe(inbox);
    expect(hashKey(queryKeys.threads({ folder: 'inbox' }, 'c2'))).not.toBe(inbox);
    expect(hashKey(queryKeys.threads({ labelId: 3 }))).not.toBe(hashKey(queryKeys.threads({ labelId: 4 })));
  });

  it('normalizes filters: q wins, then label, then folder (default inbox)', () => {
    expect(threadListFilter({ folder: 'sent', q: ' from:bob ' })).toEqual({ folder: null, labelId: null, q: 'from:bob' });
    expect(threadListFilter({ folder: 'sent', labelId: 5 })).toEqual({ folder: null, labelId: 5, q: null });
    expect(threadListFilter({})).toEqual({ folder: 'inbox', labelId: null, q: null });
  });

  it('prefix keys match their families', () => {
    const page = queryKeys.threads({ folder: 'inbox' }, 'x');
    expect(page.slice(0, 1)).toEqual(queryKeys.threadsAll());
    expect(queryKeys.thread(1).slice(0, 1)).toEqual(queryKeys.threadAll());
    // ['message', id, 'events'] shares no prefix with thread / threads keys.
    expect(queryKeys.messageEvents(1)[0]).not.toBe(queryKeys.threadAll()[0]);
    // ['thread'] must not be a prefix of ['threads', …]
    expect(queryKeys.threadAll()[0]).not.toBe(queryKeys.threadsAll()[0]);
  });

  it('exposes the stale times from the spec', () => {
    expect(staleTimes.me).toBe(300_000);
    expect(staleTimes.counts).toBe(15_000);
    expect(staleTimes.threads).toBe(30_000);
    expect(staleTimes.thread).toBe(60_000);
    expect(staleTimes.messageEvents).toBe(30_000);
    expect(staleTimes.draft).toBe(Infinity);
    expect(staleTimes.admin).toBe(0);
  });
});
