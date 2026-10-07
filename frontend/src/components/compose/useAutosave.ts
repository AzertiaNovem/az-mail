/**
 * Draft autosave [WP-F] (DESIGN.md §1 C10, §5).
 *
 * - The draft is created lazily: nothing is written until the first edit (`markDirty`).
 * - Edits are debounced (1.5 s); a save also runs on blur and before close (`flush`).
 * - The first save is POST /api/drafts. Saves are serialized, so a second POST can never be
 *   issued while the first is in flight (the duplicate-draft guard); later saves are
 *   PUT /api/drafts/:id with the current `version`.
 * - 409 `version_conflict` (another window/device saved meanwhile) stops autosaving and
 *   reports `details.current`; the window then offers 重新加载 (`acceptReload`) or 覆盖
 *   (`overwrite`, PUT with `force: true`).
 * - Other failures are retried with backoff while there are unsaved edits.
 * - Before a send, `prepareSend` waits for in-flight saves and makes sure the draft exists; the
 *   send request carries the final fields itself, so pending edits need no extra PUT.
 *
 * `DraftAutosaver` is framework-free (unit-tested with fake timers); `useAutosave` binds it to
 * the compose store (saveState / draftId) and the `['draft', id]` cache.
 */
import { useQueryClient } from '@tanstack/react-query';
import { useEffect, useLayoutEffect, useState } from 'react';
import { ApiError, isApiError } from '@/api/client';
import { createDraft, updateDraft } from '@/api/endpoints';
import { queryKeys } from '@/api/queryKeys';
import type { Draft, DraftInput, DraftUpdateInput, VersionConflictDetails } from '@/api/types';
import { useComposeStore, type ComposeSaveState } from '@/stores/compose';

export const AUTOSAVE_DEBOUNCE_MS = 1500;
const RETRY_DELAYS_MS = [3000, 10_000, 30_000, 60_000];

/** Network errors, 5xx, 408 and 429 are worth retrying; other 4xx (404 draft gone, 400, 403) are not. */
function isRetryable(e: unknown): boolean {
  if (!(e instanceof ApiError)) return true;
  return e.status === 0 || e.status >= 500 || e.status === 408 || e.status === 429;
}

export interface AutosaveApi {
  create(input: DraftInput): Promise<Draft>;
  update(draftId: number, input: DraftUpdateInput): Promise<Draft>;
}

export const defaultAutosaveApi: AutosaveApi = { create: createDraft, update: updateDraft };

export interface AutosaverCallbacks {
  /** Current field values (full DraftInput minus create-only fields). */
  collect: () => DraftInput;
  /** Create-only fields: mode, parent_message_id, include_parent_attachments. */
  createFields: () => DraftInput;
  onState?: (state: ComposeSaveState) => void;
  onSaved?: (draft: Draft, created: boolean) => void;
  onConflict?: (current: Draft | null) => void;
  onError?: (error: unknown) => void;
}

export interface DraftAutosaverOptions extends Partial<AutosaverCallbacks> {
  api?: AutosaveApi;
  debounceMs?: number;
  /** An existing draft (`{kind:'draft'}` windows, after undo / reload). */
  draftId: number | null;
  version: number;
}

const NO_INPUT = (): DraftInput => ({});

/** Thrown by `flush` / `prepareSend` while a version conflict is unresolved. */
export class DraftConflictError extends Error {
  readonly current: Draft | null;
  constructor(current: Draft | null) {
    super('version_conflict');
    this.name = 'DraftConflictError';
    this.current = current;
  }
}

type RunResult = 'ok' | 'conflict' | 'error' | 'idle';

export class DraftAutosaver {
  private readonly api: AutosaveApi;
  private readonly debounceMs: number;
  private opts: AutosaverCallbacks;
  private _draftId: number | null;
  private _version: number;
  private _dirty = false;
  private _state: ComposeSaveState;
  private timer: ReturnType<typeof setTimeout> | null = null;
  private inflight: Promise<RunResult> | null = null;
  private conflict: Draft | null | undefined = undefined; // undefined = no conflict
  private lastError: unknown = null;
  private failures = 0;
  private disposed = false;

  constructor(opts: DraftAutosaverOptions) {
    this.opts = { ...opts, collect: opts.collect ?? NO_INPUT, createFields: opts.createFields ?? NO_INPUT };
    this.api = opts.api ?? defaultAutosaveApi;
    this.debounceMs = opts.debounceMs ?? AUTOSAVE_DEBOUNCE_MS;
    this._draftId = opts.draftId;
    this._version = opts.version;
    this._state = opts.draftId === null ? 'idle' : 'saved';
  }

  get draftId(): number | null {
    return this._draftId;
  }
  get version(): number {
    return this._version;
  }
  /** Edits not yet saved (or being saved). */
  get dirty(): boolean {
    return this._dirty;
  }
  get saving(): boolean {
    return this.inflight !== null;
  }
  get state(): ComposeSaveState {
    return this._state;
  }
  get inConflict(): boolean {
    return this.conflict !== undefined;
  }

  /** Replaces the callbacks (React binds the latest closures after every render). */
  bind(callbacks: AutosaverCallbacks): void {
    this.opts = callbacks;
  }

  /** Records an edit and (re)starts the debounce. */
  markDirty(): void {
    if (this.disposed) return;
    this._dirty = true;
    if (this.inConflict) return; // nothing is saved until the conflict is resolved
    if (!this.inflight) this.setState('dirty');
    this.schedule(this.debounceMs);
  }

  /** Saves now if there are unsaved edits (blur); does not wait. */
  saveSoon(): void {
    if (this.disposed || !this._dirty || this.inConflict) return;
    this.schedule(0);
  }

  /**
   * Waits until every edit made so far is saved. Resolves with the draft id (null when nothing
   * was ever edited). Rejects with DraftConflictError on a conflict, or with the save error.
   */
  async flush(): Promise<number | null> {
    this.clearTimer();
    for (;;) {
      if (this.inConflict) throw new DraftConflictError(this.conflict ?? null);
      if (this.inflight) {
        await this.inflight;
        continue;
      }
      if (!this._dirty) return this._draftId;
      const r = await this.run();
      if (r === 'error') throw this.lastError;
    }
  }

  /**
   * Before a send: waits for in-flight saves and creates the draft if it does not exist yet.
   * Pending edits are NOT saved separately (the send request carries the final fields).
   */
  async prepareSend(): Promise<{ draftId: number; version: number }> {
    this.clearTimer();
    while (this.inflight) await this.inflight;
    if (this.inConflict) throw new DraftConflictError(this.conflict ?? null);
    if (this._draftId === null) {
      this._dirty = true;
      const r = await this.run();
      if (r === 'conflict') throw new DraftConflictError(this.conflict ?? null);
      if (r === 'error') throw this.lastError;
    }
    return { draftId: this._draftId as number, version: this._version };
  }

  /** Waits for an in-flight save without starting a new one (e.g. to learn the id before discarding). */
  async settle(): Promise<void> {
    while (this.inflight) await this.inflight;
  }

  /** A send was rejected with 409 version_conflict: stop autosaving until resolved. */
  enterConflict(current: Draft | null): void {
    this.clearTimer();
    this._dirty = true;
    this.conflict = current;
    this.setState('conflict');
  }

  /** After a failed send: resume autosaving pending edits. */
  resume(): void {
    if (this._dirty && !this.inConflict) this.schedule(this.debounceMs);
  }

  /** Conflict → 覆盖: PUT the local fields with `force: true`. */
  async overwrite(): Promise<void> {
    if (!this.inConflict) return;
    this.conflict = undefined;
    this._dirty = true;
    const r = await this.run(true);
    if (r === 'error') throw this.lastError;
  }

  /** Conflict → 重新加载: adopt the server draft (the window re-renders its fields from it). */
  acceptReload(draft: Draft): void {
    this.conflict = undefined;
    this._draftId = draft.id;
    this._version = draft.version;
    this._dirty = false;
    this.clearTimer();
    this.setState('saved');
  }

  /** Stops timers and retries (in-flight requests finish but report nothing). */
  dispose(): void {
    this.disposed = true;
    this.clearTimer();
  }

  /** Undoes `dispose` (React StrictMode re-runs effects on the same instance). */
  activate(): void {
    if (!this.disposed) return;
    this.disposed = false;
    if (this._dirty && !this.inConflict) this.schedule(this.debounceMs);
  }

  /** The last save error (null after a successful save). */
  get error(): unknown {
    return this.lastError;
  }

  // ───────────── internals ─────────────

  private setState(s: ComposeSaveState): void {
    if (this._state === s) return;
    this._state = s;
    if (!this.disposed) this.opts.onState?.(s);
  }

  private clearTimer(): void {
    if (this.timer !== null) {
      clearTimeout(this.timer);
      this.timer = null;
    }
  }

  private schedule(ms: number): void {
    this.clearTimer();
    this.timer = setTimeout(() => {
      this.timer = null;
      void this.run();
    }, ms);
  }

  private run(force = false): Promise<RunResult> {
    if (this.inflight) return this.inflight;
    if (!this._dirty || (this.inConflict && !force)) return Promise.resolve('idle');
    this.clearTimer();
    const input = this.opts.collect();
    this._dirty = false; // edits from now on mark it dirty again
    this.setState('saving');
    const p = this.save(input, force).finally(() => {
      this.inflight = null;
    });
    this.inflight = p;
    return p;
  }

  private async save(input: DraftInput, force: boolean): Promise<RunResult> {
    const creating = this._draftId === null;
    try {
      const draft = creating
        ? await this.api.create({ ...this.opts.createFields(), ...input })
        : await this.api.update(this._draftId as number, { ...input, version: this._version, ...(force ? { force: true } : {}) });
      this._draftId = draft.id;
      this._version = draft.version;
      this.failures = 0;
      this.lastError = null;
      if (!this.disposed) this.opts.onSaved?.(draft, creating);
      if (this._dirty) {
        this.setState('dirty');
        if (!this.disposed) this.schedule(this.debounceMs);
      } else {
        this.setState('saved');
      }
      return 'ok';
    } catch (e) {
      this._dirty = true; // the edits in `input` are still unsaved
      if (isApiError(e, 'version_conflict')) {
        const current = (e.detailsAs<VersionConflictDetails>().current as Draft | undefined) ?? null;
        this.conflict = current;
        this.setState('conflict');
        if (!this.disposed) this.opts.onConflict?.(current);
        return 'conflict';
      }
      this.lastError = e;
      this.failures++;
      this.setState('error');
      if (!this.disposed) {
        this.opts.onError?.(e);
        if (isRetryable(e)) this.schedule(RETRY_DELAYS_MS[Math.min(this.failures - 1, RETRY_DELAYS_MS.length - 1)] ?? 60_000);
      }
      return 'error';
    }
  }
}

// ───────────── React binding ─────────────

export interface UseAutosaveOptions {
  winKey: string;
  draftId: number | null;
  version: number;
  collect: () => DraftInput;
  createFields: () => DraftInput;
  onConflict: (current: Draft | null) => void;
  onError?: (error: unknown) => void;
  api?: AutosaveApi;
  debounceMs?: number;
}

/**
 * One DraftAutosaver per compose form (re-created only when the form remounts). Keeps the
 * window's `saveState` / `draftId` in the compose store and `['draft', id]` in the cache.
 */
export function useAutosave(opts: UseAutosaveOptions): DraftAutosaver {
  const qc = useQueryClient();
  const [saver] = useState(
    () => new DraftAutosaver({ api: opts.api, debounceMs: opts.debounceMs, draftId: opts.draftId, version: opts.version }),
  );

  // Saves only run after events, i.e. after this layout effect bound the latest closures.
  useLayoutEffect(() => {
    const { winKey } = opts;
    saver.bind({
      collect: opts.collect,
      createFields: opts.createFields,
      onState: (saveState) => useComposeStore.getState().patch(winKey, { saveState }),
      onSaved: (draft, created) => {
        qc.setQueryData(queryKeys.draft(draft.id), draft);
        if (created) {
          useComposeStore.getState().patch(winKey, { draftId: draft.id });
          void qc.invalidateQueries({ queryKey: queryKeys.counts() });
        }
      },
      onConflict: opts.onConflict,
      onError: opts.onError,
    });
  });

  useEffect(() => {
    saver.activate();
    return () => saver.dispose();
  }, [saver]);

  // Warn before leaving the page with unsaved edits.
  useEffect(() => {
    const onBeforeUnload = (e: BeforeUnloadEvent) => {
      if (saver.dirty || saver.saving) {
        e.preventDefault();
        e.returnValue = '';
      }
    };
    window.addEventListener('beforeunload', onBeforeUnload);
    return () => window.removeEventListener('beforeunload', onBeforeUnload);
  }, [saver]);

  return saver;
}
