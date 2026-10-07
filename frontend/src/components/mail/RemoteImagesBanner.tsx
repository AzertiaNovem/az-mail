import { Icon } from '@/components/common';
import { t } from '@/i18n/zh';

export interface RemoteImagesBannerProps {
  /** Sender address for "始终显示来自 x 的图片". */
  sender: string;
  /** False for spam / suspicious mail: only a one-off "显示图片". */
  canTrust: boolean;
  busy?: boolean;
  onShow: () => void;
  onAlways: () => void;
}

/** "已隐藏外部图片 · 显示图片 · 始终显示来自 x 的图片" (DESIGN.md §5 step 6). */
export function RemoteImagesBanner({ sender, canTrust, busy = false, onShow, onAlways }: RemoteImagesBannerProps) {
  return (
    <div className="mb-3 flex flex-wrap items-center gap-x-2 gap-y-1 rounded-lg bg-surface-container-low px-3 py-2 text-[13px] text-on-surface-variant" role="note">
      <Icon name="hide_image" size={18} />
      <span>{t('mail.images.hidden')}</span>
      <span aria-hidden="true">·</span>
      <button type="button" className="font-medium text-link hover:underline" onClick={onShow}>
        {t('mail.images.show')}
      </button>
      {canTrust ? (
        <>
          <span aria-hidden="true">·</span>
          <button type="button" className="min-w-0 truncate font-medium text-link hover:underline disabled:opacity-50" onClick={onAlways} disabled={busy}>
            {t('mail.images.alwaysFrom', { sender })}
          </button>
        </>
      ) : (
        <span className="w-full text-xs">{t('mail.images.spamHint')}</span>
      )}
    </div>
  );
}
