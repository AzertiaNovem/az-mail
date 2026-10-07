/**
 * Attachment uploads [WP-F]: size checks, an upload queue with progress and cancel (XHR via
 * `uploadAttachment` → api/client `uploadRaw`), and byte formatting.
 *
 * Limits (DESIGN.md §1 C12): 25 MiB per file (the server answers 413 before reading a larger
 * body) and 28 MiB of raw attachments per message, inline images included (send → 413
 * `message_too_large`). Files are rejected locally with Chinese messages before any upload.
 */
import { MAX_ATTACHMENT_BYTES, MAX_MESSAGE_ATTACHMENTS_BYTES, uploadAttachment } from '@/api/endpoints';
import { errorMessage, isAbortError } from '@/api/client';
import type { Attachment } from '@/api/types';
import { t } from '@/i18n/zh';

export { MAX_ATTACHMENT_BYTES, MAX_MESSAGE_ATTACHMENTS_BYTES };

/** Image types the server serves inline (DESIGN §1 D4); others are attached as files. */
export const INLINE_IMAGE_TYPES: ReadonlySet<string> = new Set([
  'image/png',
  'image/jpeg',
  'image/gif',
  'image/webp',
  'image/avif',
  'image/bmp',
]);

export const isInlineImage = (file: { type: string }): boolean => INLINE_IMAGE_TYPES.has(file.type.toLowerCase());

/** "0 B", "532 KB", "1.2 MB" (binary units, like Gmail). */
export function formatBytes(n: number): string {
  if (!Number.isFinite(n) || n < 0) return '0 B';
  if (n < 1024) return `${Math.round(n)} B`;
  const units = ['KB', 'MB', 'GB', 'TB'];
  let v = n / 1024;
  let u = 0;
  while (v >= 1024 && u < units.length - 1) {
    v /= 1024;
    u++;
  }
  const digits = v >= 100 || u === 0 ? 0 : 1;
  return `${v.toFixed(digits).replace(/\.0$/, '')} ${units[u]}`;
}

// ───────────── size checks ─────────────

export type SizeRejectReason = 'empty' | 'file_too_large' | 'total_too_large';

export interface SizeRejection {
  file: File;
  reason: SizeRejectReason;
}

export interface SizeCheckResult {
  accepted: File[];
  rejected: SizeRejection[];
}

export interface SizeLimits {
  file: number;
  total: number;
}

const DEFAULT_LIMITS: SizeLimits = { file: MAX_ATTACHMENT_BYTES, total: MAX_MESSAGE_ATTACHMENTS_BYTES };

/**
 * Splits `files` into accepted / rejected given the bytes already attached (or uploading).
 * Files are taken in order; one that would push the total over the limit is rejected but later
 * smaller files may still fit. Empty files (and dropped folders, which arrive as 0-byte files)
 * are rejected.
 */
export function checkUploadSizes(files: readonly File[], existingBytes: number, limits: SizeLimits = DEFAULT_LIMITS): SizeCheckResult {
  const accepted: File[] = [];
  const rejected: SizeRejection[] = [];
  let total = Math.max(0, existingBytes);
  for (const file of files) {
    if (file.size <= 0) rejected.push({ file, reason: 'empty' });
    else if (file.size > limits.file) rejected.push({ file, reason: 'file_too_large' });
    else if (total + file.size > limits.total) rejected.push({ file, reason: 'total_too_large' });
    else {
      accepted.push(file);
      total += file.size;
    }
  }
  return { accepted, rejected };
}

export function sizeRejectionMessage(r: SizeRejection): string {
  const name = r.file.name || t('compose.attachments.unnamed');
  switch (r.reason) {
    case 'empty':
      return t('compose.attachments.emptyFile', { name });
    case 'file_too_large':
      return t('compose.attachments.fileTooLarge', { name });
    case 'total_too_large':
      return t('compose.attachments.totalTooLarge', { name });
  }
}

// ───────────── queue ─────────────

export type UploadState = 'queued' | 'uploading' | 'done' | 'error' | 'canceled';

export interface UploadItem {
  /** Local id (not the attachment id). */
  id: string;
  name: string;
  size: number;
  contentType: string;
  inline: boolean;
  state: UploadState;
  loaded: number;
  /** 0..1 */
  fraction: number;
  /** Chinese message when `state === 'error'`. */
  error: string | null;
  attachment: Attachment | null;
}

export interface UploaderOptions {
  filename: string;
  inline: boolean;
  onProgress: (p: { loaded: number; total: number; fraction: number }) => void;
  signal: AbortSignal;
}

export type Uploader = (file: File, opts: UploaderOptions) => Promise<Attachment>;

const defaultUploader: Uploader = (file, opts) =>
  uploadAttachment(file, { filename: opts.filename, inline: opts.inline, onProgress: opts.onProgress, signal: opts.signal });

export interface AddOptions {
  inline: boolean;
  /** Called once per file that finished uploading. */
  onDone?: (item: UploadItem, attachment: Attachment) => void;
  /** Called once per file that failed (not for cancellations). */
  onError?: (item: UploadItem) => void;
}

interface Entry {
  item: UploadItem;
  file: File;
  controller: AbortController;
  opts: AddOptions;
}

export interface UploadQueueOptions {
  uploader?: Uploader;
  /** Parallel uploads (default 3). */
  concurrency?: number;
}

let seq = 0;

/**
 * Upload queue of one compose window. Items stay listed while queued / uploading / failed;
 * finished items are handed to `onDone` and leave the list (the window then owns the
 * Attachment). `cancel` aborts an upload (or drops a failed item). Listeners get a fresh
 * snapshot array on every change.
 */
export class UploadQueue {
  private readonly uploader: Uploader;
  private readonly concurrency: number;
  private readonly entries = new Map<string, Entry>();
  private readonly listeners = new Set<(items: UploadItem[]) => void>();
  private active = 0;
  private disposed = false;
  private snapshot: UploadItem[] = [];

  constructor(opts: UploadQueueOptions = {}) {
    this.uploader = opts.uploader ?? defaultUploader;
    this.concurrency = Math.max(1, opts.concurrency ?? 3);
  }

  /** Current items (stable array until the next change). */
  list(): UploadItem[] {
    return this.snapshot;
  }

  /** Queued + uploading count (Send is disabled while > 0). */
  pendingCount(): number {
    let n = 0;
    for (const e of this.entries.values()) if (e.item.state === 'queued' || e.item.state === 'uploading') n++;
    return n;
  }

  /** Bytes of queued / uploading items (count against the 28 MiB message limit). */
  pendingBytes(): number {
    let n = 0;
    for (const e of this.entries.values()) if (e.item.state === 'queued' || e.item.state === 'uploading') n += e.item.size;
    return n;
  }

  subscribe(fn: (items: UploadItem[]) => void): () => void {
    this.listeners.add(fn);
    return () => this.listeners.delete(fn);
  }

  add(files: readonly File[], opts: AddOptions): UploadItem[] {
    if (this.disposed) return [];
    const added: UploadItem[] = [];
    for (const file of files) {
      const item: UploadItem = {
        id: `up${++seq}`,
        name: file.name || t('compose.attachments.unnamed'),
        size: file.size,
        contentType: file.type || 'application/octet-stream',
        inline: opts.inline,
        state: 'queued',
        loaded: 0,
        fraction: 0,
        error: null,
        attachment: null,
      };
      this.entries.set(item.id, { item, file, controller: new AbortController(), opts });
      added.push(item);
    }
    this.emit();
    this.pump();
    return added;
  }

  /** Aborts an upload in flight / drops a queued or failed item. */
  cancel(id: string): void {
    const e = this.entries.get(id);
    if (!e) return;
    this.entries.delete(id);
    if (e.item.state === 'uploading' || e.item.state === 'queued') e.controller.abort();
    e.item = { ...e.item, state: 'canceled' };
    this.emit();
    this.pump();
  }

  /** Cancels every queued / running / failed item (the queue stays usable). */
  cancelAll(): void {
    for (const id of [...this.entries.keys()]) this.cancel(id);
  }

  /** Cancels everything; later `add`s are ignored. */
  dispose(): void {
    this.disposed = true;
    for (const id of [...this.entries.keys()]) this.cancel(id);
    this.listeners.clear();
  }

  private update(id: string, patch: Partial<UploadItem>): void {
    const e = this.entries.get(id);
    if (!e) return;
    e.item = { ...e.item, ...patch };
    this.emit();
  }

  private emit(): void {
    this.snapshot = [...this.entries.values()].map((e) => e.item);
    for (const l of [...this.listeners]) l(this.snapshot);
  }

  private pump(): void {
    if (this.disposed) return;
    for (const e of this.entries.values()) {
      if (this.active >= this.concurrency) return;
      if (e.item.state === 'queued') void this.run(e);
    }
  }

  private async run(e: Entry): Promise<void> {
    const id = e.item.id;
    this.active++;
    this.update(id, { state: 'uploading' });
    try {
      const att = await this.uploader(e.file, {
        filename: e.item.name,
        inline: e.item.inline,
        signal: e.controller.signal,
        onProgress: (p) => this.update(id, { loaded: p.loaded, fraction: p.fraction }),
      });
      if (!this.entries.has(id)) return; // canceled while finishing
      const done: UploadItem = { ...e.item, state: 'done', loaded: e.item.size, fraction: 1, attachment: att };
      this.entries.delete(id);
      this.emit();
      e.opts.onDone?.(done, att);
    } catch (err) {
      if (!this.entries.has(id) || isAbortError(err)) return;
      this.update(id, { state: 'error', error: errorMessage(err) });
      const failed = this.entries.get(id)?.item;
      if (failed) e.opts.onError?.(failed);
    } finally {
      this.active--;
      this.pump();
    }
  }
}
