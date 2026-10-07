import { useState } from 'react';
import type { Attachment } from '@/api/types';
import { resolveApiUrl } from '@/config';
import { Button, Dialog, Icon } from '@/components/common';
import { t } from '@/i18n/zh';
import { formatBytes } from '@/lib/format';
import { fileIconFor, isImageType } from './fileIcon';

export interface AttachmentPreviewProps {
  attachment: Attachment | null;
  onClose: () => void;
}

/** Full-size preview of an image attachment (other types open in a new tab instead). */
export function AttachmentPreview({ attachment, onClose }: AttachmentPreviewProps) {
  const [failed, setFailed] = useState<number | null>(null);
  const a = attachment;
  const canShow = a !== null && a.view_url !== null && isImageType(a.content_type) && failed !== a.id;
  const fi = a ? fileIconFor(a.content_type, a.filename) : null;
  return (
    <Dialog
      open={a !== null}
      onOpenChange={(o) => {
        if (!o) onClose();
      }}
      title={a ? a.filename : t('mail.attachments.previewTitle')}
      description={a ? `${fi?.kind ?? ''} · ${formatBytes(a.size)}` : undefined}
      size="xl"
      footer={
        a && (
          <>
            {a.view_url && (
              <Button variant="text" icon="open_in_new" onClick={() => window.open(resolveApiUrl(a.view_url!), '_blank', 'noopener,noreferrer')}>
                {t('mail.attachments.open')}
              </Button>
            )}
            <a
              href={resolveApiUrl(a.download_url)}
              download={a.filename}
              rel="noopener noreferrer"
              className="inline-flex h-9 items-center gap-2 rounded-full bg-primary px-4 text-sm font-medium text-on-primary hover:bg-primary-hover"
            >
              <Icon name="download" size={18} />
              {t('actions.download')}
            </a>
          </>
        )
      }
    >
      {a && (
        <div className="flex min-h-[200px] items-center justify-center rounded-lg bg-surface-container-low p-2">
          {canShow ? (
            <img
              src={resolveApiUrl(a.view_url!)}
              alt={a.filename}
              referrerPolicy="no-referrer"
              onError={() => setFailed(a.id)}
              className="max-h-[70vh] max-w-full object-contain"
            />
          ) : (
            <div className="flex flex-col items-center gap-2 text-on-surface-variant">
              <Icon name={fi?.icon ?? 'draft'} size={48} style={{ color: fi?.color }} />
              <span className="text-sm">{t('mail.attachments.noPreview')}</span>
            </div>
          )}
        </div>
      )}
    </Dialog>
  );
}
