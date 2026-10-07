import { describe, expect, it, vi } from 'vitest';
import { ApiError } from '@/api/client';
import type { Attachment } from '@/api/types';
import { fakeFile, makeAttachment } from '@/components/compose/testFixtures';
import {
  checkUploadSizes,
  formatBytes,
  isInlineImage,
  MAX_ATTACHMENT_BYTES,
  MAX_MESSAGE_ATTACHMENTS_BYTES,
  sizeRejectionMessage,
  UploadQueue,
  type Uploader,
  type UploaderOptions,
} from './upload';

const MiB = 1024 * 1024;

function deferred<T>() {
  let resolve!: (v: T) => void;
  let reject!: (e: unknown) => void;
  const promise = new Promise<T>((res, rej) => {
    resolve = res;
    reject = rej;
  });
  return { promise, resolve, reject };
}

/** Uploader whose calls are resolved manually. */
function manualUploader() {
  const calls: { file: File; opts: UploaderOptions; d: ReturnType<typeof deferred<Attachment>> }[] = [];
  const uploader: Uploader = (file, opts) => {
    const d = deferred<Attachment>();
    opts.signal.addEventListener('abort', () => d.reject(new DOMException('Aborted', 'AbortError')));
    calls.push({ file, opts, d });
    return d.promise;
  };
  return { uploader, calls };
}

const tick = () => new Promise((r) => setTimeout(r, 0));

describe('size limits', () => {
  it('uses the 25 MiB / 28 MiB limits', () => {
    expect(MAX_ATTACHMENT_BYTES).toBe(25 * MiB);
    expect(MAX_MESSAGE_ATTACHMENTS_BYTES).toBe(28 * MiB);
  });

  it('rejects files over 25 MiB, empty files, and files that overflow 28 MiB in total', () => {
    const big = fakeFile('big.zip', 25 * MiB + 1);
    const exact = fakeFile('exact.bin', 25 * MiB);
    const empty = fakeFile('folder', 0);
    const r1 = checkUploadSizes([big, exact, empty], 0);
    expect(r1.accepted).toEqual([exact]);
    expect(r1.rejected.map((r) => [r.file.name, r.reason])).toEqual([
      ['big.zip', 'file_too_large'],
      ['folder', 'empty'],
    ]);

    // 20 MiB already attached: an 8 MiB file overflows, a later 7 MiB one still fits... once.
    const r2 = checkUploadSizes([fakeFile('a', 9 * MiB), fakeFile('b', 7 * MiB), fakeFile('c', 2 * MiB)], 20 * MiB);
    expect(r2.accepted.map((f) => f.name)).toEqual(['b']);
    expect(r2.rejected.map((r) => r.reason)).toEqual(['total_too_large', 'total_too_large']);
  });

  it('explains rejections in Chinese', () => {
    expect(sizeRejectionMessage({ file: fakeFile('报告.pdf', 1), reason: 'file_too_large' })).toBe('“报告.pdf”超过了 25 MB 的单个附件大小上限');
    expect(sizeRejectionMessage({ file: fakeFile('x', 1), reason: 'total_too_large' })).toContain('28 MB');
    expect(sizeRejectionMessage({ file: fakeFile('', 0), reason: 'empty' })).toBe('无法添加空文件或文件夹“未命名文件”');
  });

  it('formats bytes and recognises inline image types', () => {
    expect(formatBytes(0)).toBe('0 B');
    expect(formatBytes(1023)).toBe('1023 B');
    expect(formatBytes(1536)).toBe('2 KB');
    expect(formatBytes(1.5 * MiB)).toBe('1.5 MB');
    expect(formatBytes(25 * MiB)).toBe('25 MB');
    expect(formatBytes(-1)).toBe('0 B');
    expect(isInlineImage({ type: 'image/PNG' })).toBe(true);
    expect(isInlineImage({ type: 'image/svg+xml' })).toBe(false);
    expect(isInlineImage({ type: 'application/pdf' })).toBe(false);
  });
});

describe('UploadQueue', () => {
  it('uploads with progress and hands finished attachments to onDone', async () => {
    const { uploader, calls } = manualUploader();
    const q = new UploadQueue({ uploader });
    const snapshots: string[][] = [];
    q.subscribe((items) => snapshots.push(items.map((i) => `${i.name}:${i.state}:${Math.round(i.fraction * 100)}`)));
    const onDone = vi.fn();
    const [item] = q.add([fakeFile('a.png', 100, 'image/png')], { inline: true, onDone });
    expect(item!.inline).toBe(true);
    expect(q.pendingCount()).toBe(1);
    expect(q.pendingBytes()).toBe(100);
    expect(calls[0]!.opts).toMatchObject({ filename: 'a.png', inline: true });

    calls[0]!.opts.onProgress({ loaded: 50, total: 100, fraction: 0.5 });
    expect(q.list()[0]).toMatchObject({ state: 'uploading', fraction: 0.5, loaded: 50 });

    const att = makeAttachment({ id: 9, inline: true });
    calls[0]!.d.resolve(att);
    await tick();
    expect(onDone).toHaveBeenCalledWith(expect.objectContaining({ state: 'done', attachment: att }), att);
    expect(q.list()).toEqual([]);
    expect(q.pendingCount()).toBe(0);
    expect(snapshots).toContainEqual(['a.png:uploading:50']);
  });

  it('limits concurrency and starts queued uploads as slots free up', async () => {
    const { uploader, calls } = manualUploader();
    const q = new UploadQueue({ uploader, concurrency: 2 });
    q.add([fakeFile('1', 1), fakeFile('2', 1), fakeFile('3', 1)], { inline: false });
    expect(calls).toHaveLength(2);
    expect(q.list().map((i) => i.state)).toEqual(['uploading', 'uploading', 'queued']);
    calls[0]!.d.resolve(makeAttachment({ id: 1 }));
    await tick();
    expect(calls).toHaveLength(3);
  });

  it('cancel aborts an upload without reporting an error', async () => {
    const { uploader, calls } = manualUploader();
    const q = new UploadQueue({ uploader });
    const onDone = vi.fn();
    const onError = vi.fn();
    const [item] = q.add([fakeFile('a', 1)], { inline: false, onDone, onError });
    q.cancel(item!.id);
    expect(calls[0]!.opts.signal.aborted).toBe(true);
    await tick();
    expect(q.list()).toEqual([]);
    expect(onDone).not.toHaveBeenCalled();
    expect(onError).not.toHaveBeenCalled();
  });

  it('keeps failed uploads listed with a Chinese error until dismissed', async () => {
    const { uploader, calls } = manualUploader();
    const q = new UploadQueue({ uploader });
    const onError = vi.fn();
    q.add([fakeFile('a', 1)], { inline: false, onError });
    calls[0]!.d.reject(new ApiError(503, 'storage_unavailable', '文件存储暂时不可用'));
    await tick();
    expect(q.list()[0]).toMatchObject({ state: 'error', error: '文件存储暂时不可用' });
    expect(onError).toHaveBeenCalledTimes(1);
    expect(q.pendingCount()).toBe(0);
    q.cancel(q.list()[0]!.id);
    expect(q.list()).toEqual([]);
  });

  it('dispose cancels everything and ignores later adds; cancelAll keeps it usable', async () => {
    const { uploader, calls } = manualUploader();
    const q = new UploadQueue({ uploader });
    q.add([fakeFile('a', 1), fakeFile('b', 1)], { inline: false });
    q.cancelAll();
    expect(q.list()).toEqual([]);
    expect(calls.every((c) => c.opts.signal.aborted)).toBe(true);
    expect(q.add([fakeFile('c', 1)], { inline: false })).toHaveLength(1);
    q.dispose();
    expect(q.add([fakeFile('d', 1)], { inline: false })).toEqual([]);
  });
});
