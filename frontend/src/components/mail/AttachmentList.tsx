/**
 * Attachment cards under a message [WP-E] (Gmail 2024): image thumbnails via `view_url`, a
 * file-type icon otherwise, name + size, and download / preview on hover or focus. Inline
 * images that the body already shows are not repeated.
 */
import { useState } from 'react';
import type { Attachment, Message } from '@/api/types';
import { resolveApiUrl } from '@/config';
import { Icon, Tooltip } from '@/components/common';
import { t } from '@/i18n/zh';
import { formatBytes } from '@/lib/format';
import { fileIconFor, isImageType } from './fileIcon';

/** Attachments to list: everything except inline images referenced by the HTML body. */
export function listedAttachments(message: Pick<Message, 'attachments' | 'html'>): Attachment[] {
  const html = message.html ?? '';
  return message.attachments.filter((a) => {
    if (!a.inline || !html) return true;
    const referenced =
      html.includes(`/api/files/${a.id}?`) || (a.content_id !== null && a.content_id !== '' && html.includes(`cid:${a.content_id}`));
    return !referenced;
  });
}

function Thumb({ a }: { a: Attachment }) {
  const [broken, setBroken] = useState(false);
  const fi = fileIconFor(a.content_type, a.filename);
  if (a.view_url && isImageType(a.content_type) && !broken) {
    return (
      <img
        src={resolveApiUrl(a.view_url)}
        alt=""
        loading="lazy"
        referrerPolicy="no-referrer"
        onError={() => setBroken(true)}
        className="size-full object-cover"
      />
    );
  }
  return (
    <span className="flex size-full items-center justify-center" style={{ backgroundColor: `${fi.color}14` }}>
      <Icon name={fi.icon} size={40} style={{ color: fi.color }} />
    </span>
  );
}

export interface AttachmentListProps {
  attachments: Attachment[];
  onPreview: (a: Attachment) => void;
}

export function AttachmentList({ attachments, onPreview }: AttachmentListProps) {
  if (attachments.length === 0) return null;
  return (
    <section className="mt-4 border-t border-divider pt-4" aria-label={t('mail.attachments.title', { count: attachments.length })}>
      <h3 className="mb-3 text-sm font-medium text-on-surface-variant">{t('mail.attachments.title', { count: attachments.length })}</h3>
      <ul className="flex flex-wrap gap-3">
        {attachments.map((a) => {
          const fi = fileIconFor(a.content_type, a.filename);
          const previewable = a.view_url !== null;
          return (
            <li
              key={a.id}
              className="group relative h-[124px] w-[176px] overflow-hidden rounded-lg border border-outline-variant bg-surface-container transition-shadow hover:shadow-elevation-1"
            >
              <button
                type="button"
                className="block h-[84px] w-full overflow-hidden bg-surface-container-low"
                onClick={() => (previewable ? onPreview(a) : window.open(resolveApiUrl(a.download_url), '_blank', 'noopener,noreferrer'))}
                aria-label={t('mail.attachments.preview', { name: a.filename })}
              >
                <Thumb a={a} />
              </button>
              <div className="flex h-10 items-center gap-2 border-t border-outline-variant/60 px-2.5">
                <Icon name={fi.icon} size={18} style={{ color: fi.color }} />
                <span className="min-w-0 flex-1 truncate text-xs text-on-surface" title={a.filename}>
                  {a.filename}
                </span>
              </div>
              <div className="pointer-events-none absolute inset-0 flex flex-col justify-between bg-[#202124]/70 p-2.5 text-white opacity-0 transition-opacity group-focus-within:pointer-events-auto group-focus-within:opacity-100 group-hover:pointer-events-auto group-hover:opacity-100">
                <div className="min-w-0">
                  <p className="truncate text-xs font-medium">{a.filename}</p>
                  <p className="text-[11px] opacity-80">{formatBytes(a.size)}</p>
                </div>
                <div className="flex justify-end gap-1">
                  {previewable && (
                    <Tooltip label={t('mail.attachments.preview', { name: a.filename })}>
                      <button
                        type="button"
                        onClick={() => onPreview(a)}
                        aria-label={t('mail.attachments.preview', { name: a.filename })}
                        className="flex size-8 items-center justify-center rounded-full bg-white/15 hover:bg-white/25"
                      >
                        <Icon name="visibility" size={18} />
                      </button>
                    </Tooltip>
                  )}
                  <Tooltip label={t('mail.attachments.download', { name: a.filename })}>
                    <a
                      href={resolveApiUrl(a.download_url)}
                      download={a.filename}
                      rel="noopener noreferrer"
                      aria-label={t('mail.attachments.download', { name: a.filename })}
                      className="flex size-8 items-center justify-center rounded-full bg-white/15 hover:bg-white/25"
                    >
                      <Icon name="download" size={18} />
                    </a>
                  </Tooltip>
                </div>
              </div>
            </li>
          );
        })}
      </ul>
    </section>
  );
}
