/**
 * Attachment chips under the compose body [WP-F]: uploaded attachments (name, size, remove) and
 * uploads in progress (progress bar, cancel) or failed (red, dismiss). Inline images that made
 * it into the body are not listed (they are visible in the body).
 */
import type { Attachment } from '@/api/types';
import { cx, Icon, Tooltip } from '@/components/common';
import { resolveApiUrl } from '@/config';
import { t } from '@/i18n/zh';
import { formatBytes, type UploadItem } from '@/lib/upload';

export function fileIcon(contentType: string, filename: string): string {
  const type = contentType.toLowerCase();
  const ext = filename.toLowerCase().split('.').pop() ?? '';
  if (type.startsWith('image/')) return 'image';
  if (type.startsWith('video/')) return 'movie';
  if (type.startsWith('audio/')) return 'music_note';
  if (type === 'application/pdf' || ext === 'pdf') return 'picture_as_pdf';
  if (/zip|rar|7z|tar|gzip/.test(type) || ['zip', 'rar', '7z', 'gz', 'tgz'].includes(ext)) return 'folder_zip';
  if (/sheet|excel|csv/.test(type) || ['xls', 'xlsx', 'csv'].includes(ext)) return 'table_chart';
  if (/presentation|powerpoint/.test(type) || ['ppt', 'pptx', 'key'].includes(ext)) return 'slideshow';
  return 'description';
}

export interface AttachmentBarProps {
  attachments: readonly Attachment[];
  uploads: readonly UploadItem[];
  onRemove: (attachmentId: number) => void;
  /** Cancel an upload in flight, or dismiss a failed one. */
  onCancelUpload: (uploadId: string) => void;
  disabled?: boolean;
}

export function AttachmentBar({ attachments, uploads, onRemove, onCancelUpload, disabled }: AttachmentBarProps) {
  if (attachments.length === 0 && uploads.length === 0) return null;
  return (
    <ul className="cw-attachments" aria-label={t('compose.attachments.label')}>
      {attachments.map((a) => (
        <li key={`a${a.id}`} className="cw-attachment">
          <Icon name={fileIcon(a.content_type, a.filename)} size={18} className="text-on-surface-variant" />
          <a
            className="cw-attachment-name"
            href={resolveApiUrl(a.view_url ?? a.download_url)}
            target="_blank"
            rel="noopener noreferrer"
            title={a.filename}
          >
            {a.filename}
          </a>
          <span className="cw-attachment-size">({formatBytes(a.size)})</span>
          <button
            type="button"
            className="cw-attachment-remove"
            aria-label={t('compose.attachments.remove', { name: a.filename })}
            disabled={disabled}
            onClick={() => onRemove(a.id)}
          >
            <Icon name="close" size={16} />
          </button>
        </li>
      ))}
      {uploads.map((u) => {
        const failed = u.state === 'error';
        const percent = Math.round(u.fraction * 100);
        return (
          <li key={u.id} className={cx('cw-attachment', failed && 'is-error')} aria-busy={!failed || undefined}>
            <Icon name={failed ? 'error' : fileIcon(u.contentType, u.name)} size={18} className={failed ? 'text-error' : 'text-on-surface-variant'} />
            <span className="cw-attachment-name is-pending" title={u.name}>
              {u.name}
            </span>
            {failed ? (
              <Tooltip label={u.error ?? t('compose.attachments.failed')}>
                <span className="cw-attachment-size text-error">{t('compose.attachments.failed')}</span>
              </Tooltip>
            ) : (
              <span className="cw-attachment-size">({formatBytes(u.size)})</span>
            )}
            <button
              type="button"
              className="cw-attachment-remove"
              aria-label={t('compose.attachments.cancel', { name: u.name })}
              onClick={() => onCancelUpload(u.id)}
            >
              <Icon name="close" size={16} />
            </button>
            {!failed && (
              <span
                className="cw-progress"
                role="progressbar"
                aria-label={t('compose.attachments.uploading', { percent })}
                aria-valuemin={0}
                aria-valuemax={100}
                aria-valuenow={percent}
              >
                <span className="cw-progress-bar" style={{ width: `${Math.max(4, percent)}%` }} />
              </span>
            )}
          </li>
        );
      })}
    </ul>
  );
}
