import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { ApiError } from '@/api/client';
import type { Draft, DraftInput, DraftUpdateInput } from '@/api/types';
import type { ComposeSaveState } from '@/stores/compose';
import { makeDraft } from './testFixtures';
import { AUTOSAVE_DEBOUNCE_MS, DraftAutosaver, DraftConflictError, type AutosaveApi } from './useAutosave';

function deferred<T>() {
  let resolve!: (v: T) => void;
  let reject!: (e: unknown) => void;
  const promise = new Promise<T>((res, rej) => {
    resolve = res;
    reject = rej;
  });
  return { promise, resolve, reject };
}

/** API double recording calls; each call returns a promise resolved by the test (or auto). */
function fakeApi(auto = true) {
  const creates: { input: DraftInput; d: ReturnType<typeof deferred<Draft>> }[] = [];
  const updates: { id: number; input: DraftUpdateInput; d: ReturnType<typeof deferred<Draft>> }[] = [];
  let version = 1;
  const api: AutosaveApi = {
    create: (input) => {
      const d = deferred<Draft>();
      creates.push({ input, d });
      if (auto) d.resolve(makeDraft({ id: 42, version: 1, subject: input.subject ?? '' }));
      return d.promise;
    },
    update: (id, input) => {
      const d = deferred<Draft>();
      updates.push({ id, input, d });
      if (auto) d.resolve(makeDraft({ id, version: ++version, subject: input.subject ?? '' }));
      return d.promise;
    },
  };
  return { api, creates, updates };
}

describe('DraftAutosaver', () => {
  let subject: string;
  let states: ComposeSaveState[];
  beforeEach(() => {
    vi.useFakeTimers();
    subject = 's0';
    states = [];
  });
  afterEach(() => {
    vi.useRealTimers();
  });

  function make(api: AutosaveApi, extra: Partial<ConstructorParameters<typeof DraftAutosaver>[0]> = {}) {
    return new DraftAutosaver({
      api,
      draftId: null,
      version: 0,
      collect: () => ({ subject, html: '<p>x</p>' }),
      createFields: () => ({ mode: 'reply', parent_message_id: 7 }),
      onState: (s) => states.push(s),
      ...extra,
    });
  }

  it('creates nothing until the first edit, then POSTs once after the 1.5 s debounce', async () => {
    const { api, creates } = fakeApi();
    const saver = make(api);
    await vi.advanceTimersByTimeAsync(10_000);
    expect(creates).toHaveLength(0);
    expect(saver.state).toBe('idle');

    saver.markDirty();
    subject = 's1';
    await vi.advanceTimersByTimeAsync(AUTOSAVE_DEBOUNCE_MS - 1);
    saver.markDirty(); // typing restarts the debounce
    await vi.advanceTimersByTimeAsync(AUTOSAVE_DEBOUNCE_MS - 1);
    expect(creates).toHaveLength(0);
    await vi.advanceTimersByTimeAsync(1);
    expect(creates).toHaveLength(1);
    // Create-only fields are merged into the first POST.
    expect(creates[0]!.input).toEqual({ mode: 'reply', parent_message_id: 7, subject: 's1', html: '<p>x</p>' });
    await vi.advanceTimersByTimeAsync(0);
    expect(saver.draftId).toBe(42);
    expect(saver.version).toBe(1);
    expect(states).toEqual(['dirty', 'saving', 'saved']);
  });

  it('never issues a second POST while the first is in flight; later saves PUT with the version', async () => {
    const { api, creates, updates } = fakeApi(false);
    const saver = make(api);
    saver.markDirty();
    await vi.advanceTimersByTimeAsync(AUTOSAVE_DEBOUNCE_MS);
    expect(creates).toHaveLength(1);

    subject = 's2';
    saver.markDirty();
    saver.saveSoon();
    await vi.advanceTimersByTimeAsync(AUTOSAVE_DEBOUNCE_MS * 3);
    expect(creates).toHaveLength(1); // duplicate-POST guard
    expect(updates).toHaveLength(0);

    creates[0]!.d.resolve(makeDraft({ id: 42, version: 1 }));
    await vi.advanceTimersByTimeAsync(0);
    expect(saver.state).toBe('dirty');
    await vi.advanceTimersByTimeAsync(AUTOSAVE_DEBOUNCE_MS);
    expect(updates).toHaveLength(1);
    expect(updates[0]).toMatchObject({ id: 42, input: { subject: 's2', version: 1 } });
    updates[0]!.d.resolve(makeDraft({ id: 42, version: 2 }));
    await vi.advanceTimersByTimeAsync(0);
    expect(saver.version).toBe(2);
    expect(saver.dirty).toBe(false);
  });

  it('flush waits for every edit, including edits made during an in-flight save', async () => {
    const { api, creates, updates } = fakeApi(false);
    const saver = make(api, { draftId: 42, version: 3 });
    saver.markDirty();
    await vi.advanceTimersByTimeAsync(AUTOSAVE_DEBOUNCE_MS);
    expect(updates).toHaveLength(1);
    subject = 'late';
    saver.markDirty();
    const flushed = saver.flush();
    updates[0]!.d.resolve(makeDraft({ id: 42, version: 4 }));
    await vi.advanceTimersByTimeAsync(0);
    expect(updates).toHaveLength(2);
    expect(updates[1]!.input).toMatchObject({ subject: 'late', version: 4 });
    updates[1]!.d.resolve(makeDraft({ id: 42, version: 5 }));
    await expect(flushed).resolves.toBe(42);
    expect(creates).toHaveLength(0);
    expect(saver.version).toBe(5);
  });

  it('flush on a never-edited window resolves null without requests', async () => {
    const { api, creates } = fakeApi();
    await expect(make(api).flush()).resolves.toBeNull();
    expect(creates).toHaveLength(0);
  });

  it('stops on 409 version_conflict and resolves by overwrite (force) or reload', async () => {
    const { api, updates } = fakeApi(false);
    const current = makeDraft({ id: 42, version: 9, subject: '别处的修改' });
    const onConflict = vi.fn();
    const saver = make(api, { draftId: 42, version: 3, onConflict });
    saver.markDirty();
    await vi.advanceTimersByTimeAsync(AUTOSAVE_DEBOUNCE_MS);
    updates[0]!.d.reject(new ApiError(409, 'version_conflict', '冲突', { current }));
    await vi.advanceTimersByTimeAsync(0);
    expect(onConflict).toHaveBeenCalledWith(current);
    expect(saver.state).toBe('conflict');
    expect(saver.inConflict).toBe(true);

    // No autosave while in conflict, and flush rejects.
    saver.markDirty();
    await vi.advanceTimersByTimeAsync(AUTOSAVE_DEBOUNCE_MS * 4);
    expect(updates).toHaveLength(1);
    await expect(saver.flush()).rejects.toBeInstanceOf(DraftConflictError);

    // 覆盖 → PUT with force and the version we have.
    const done = saver.overwrite();
    await vi.advanceTimersByTimeAsync(0);
    expect(updates[1]!.input).toMatchObject({ version: 3, force: true });
    updates[1]!.d.resolve(makeDraft({ id: 42, version: 10 }));
    await done;
    expect(saver.inConflict).toBe(false);
    expect(saver.version).toBe(10);
  });

  it('重新加载 adopts the server version', async () => {
    const { api, updates } = fakeApi(false);
    const saver = make(api, { draftId: 42, version: 3 });
    saver.markDirty();
    await vi.advanceTimersByTimeAsync(AUTOSAVE_DEBOUNCE_MS);
    updates[0]!.d.reject(new ApiError(409, 'version_conflict', '冲突', {}));
    await vi.advanceTimersByTimeAsync(0);
    saver.acceptReload(makeDraft({ id: 42, version: 12 }));
    expect(saver).toMatchObject({ inConflict: false, dirty: false, version: 12, state: 'saved' });
  });

  it('retries transient failures with backoff but not 4xx errors', async () => {
    const { api, updates } = fakeApi(false);
    const onError = vi.fn();
    const saver = make(api, { draftId: 42, version: 1, onError });
    saver.markDirty();
    await vi.advanceTimersByTimeAsync(AUTOSAVE_DEBOUNCE_MS);
    updates[0]!.d.reject(new ApiError(0, 'network_error', '网络连接失败'));
    await vi.advanceTimersByTimeAsync(0);
    expect(saver.state).toBe('error');
    expect(saver.dirty).toBe(true);
    await vi.advanceTimersByTimeAsync(3000);
    expect(updates).toHaveLength(2);
    updates[1]!.d.reject(new ApiError(404, 'not_found', '草稿不存在'));
    await vi.advanceTimersByTimeAsync(0);
    await vi.advanceTimersByTimeAsync(120_000);
    expect(updates).toHaveLength(2);
    expect(onError).toHaveBeenCalledTimes(2);
    // An explicit flush (close) tries once more and reports the failure.
    const flushed = saver.flush();
    await vi.advanceTimersByTimeAsync(0);
    expect(updates).toHaveLength(3);
    updates[2]!.d.reject(new ApiError(404, 'not_found', '草稿不存在'));
    await expect(flushed).rejects.toMatchObject({ code: 'not_found' });
  });

  it('prepareSend creates the draft when needed and waits for in-flight saves', async () => {
    const { api, creates, updates } = fakeApi(false);
    const saver = make(api);
    const p = saver.prepareSend();
    await vi.advanceTimersByTimeAsync(0);
    expect(creates).toHaveLength(1);
    creates[0]!.d.resolve(makeDraft({ id: 77, version: 1 }));
    await expect(p).resolves.toEqual({ draftId: 77, version: 1 });

    // A pending (debounced) edit is not PUT separately: the send carries the fields.
    saver.markDirty();
    await expect(saver.prepareSend()).resolves.toEqual({ draftId: 77, version: 1 });
    await vi.advanceTimersByTimeAsync(AUTOSAVE_DEBOUNCE_MS * 2);
    expect(updates).toHaveLength(0);
    // After a failed send, autosave resumes.
    saver.resume();
    await vi.advanceTimersByTimeAsync(AUTOSAVE_DEBOUNCE_MS);
    expect(updates).toHaveLength(1);
  });

  it('enterConflict (409 on send) blocks saving; dispose stops timers; activate resumes', async () => {
    const { api, updates } = fakeApi();
    const saver = make(api, { draftId: 1, version: 1 });
    saver.enterConflict(null);
    saver.markDirty();
    await vi.advanceTimersByTimeAsync(AUTOSAVE_DEBOUNCE_MS * 2);
    expect(updates).toHaveLength(0);
    await expect(saver.prepareSend()).rejects.toBeInstanceOf(DraftConflictError);

    const other = make(api, { draftId: 2, version: 1 });
    other.markDirty();
    other.dispose();
    await vi.advanceTimersByTimeAsync(AUTOSAVE_DEBOUNCE_MS * 2);
    expect(updates).toHaveLength(0);
    other.activate();
    await vi.advanceTimersByTimeAsync(AUTOSAVE_DEBOUNCE_MS);
    expect(updates).toHaveLength(1);
  });
});
