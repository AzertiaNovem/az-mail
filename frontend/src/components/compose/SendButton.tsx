/**
 * Gmail's blue split "发送" button [WP-F]: the main part sends; the arrow opens a menu with
 * "定时发送".
 */
import { cx, DropdownMenu, Icon, Spinner, Tooltip } from '@/components/common';
import { t } from '@/i18n/zh';
import { shortcut } from './EditorToolbar';

export interface SendButtonProps {
  onSend: () => void;
  onSchedule: () => void;
  disabled?: boolean;
  /** Why it is disabled (tooltip), e.g. uploads pending. */
  disabledReason?: string;
  sending?: boolean;
}

export function SendButton({ onSend, onSchedule, disabled, disabledReason, sending }: SendButtonProps) {
  const off = disabled || sending;
  const main = (
    <button type="button" className="cw-send-main" onClick={onSend} disabled={off} aria-busy={sending || undefined}>
      {sending && <Spinner size={16} inheritColor decorative />}
      {sending ? t('compose.sending') : t('compose.send')}
    </button>
  );
  return (
    <div className={cx('cw-send', off && 'is-disabled')} role="group" aria-label={t('compose.send')}>
      {off && disabledReason ? (
        <Tooltip label={disabledReason} side="top">
          <span className="inline-flex">{main}</span>
        </Tooltip>
      ) : (
        <Tooltip label={t('compose.send')} shortcut={shortcut('Enter')} side="top">
          {main}
        </Tooltip>
      )}
      <DropdownMenu
        side="top"
        align="start"
        aria-label={t('compose.moreSendOptions')}
        trigger={
          <button type="button" className="cw-send-more" aria-label={t('compose.moreSendOptions')} disabled={off}>
            <Icon name="arrow_drop_down" size={20} />
          </button>
        }
        items={[{ key: 'schedule', label: t('compose.scheduleSend'), icon: 'schedule_send', onSelect: onSchedule }]}
      />
    </div>
  );
}
