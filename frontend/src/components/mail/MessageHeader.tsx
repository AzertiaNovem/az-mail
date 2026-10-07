/**
 * Expanded message header [WP-E]: avatar, sender name + <email>, "发送至 我 ▾" with the details
 * popover (发件人 / 回复地址 / 收件人 / 抄送 / 密送 / 日期 / 主题 / 送达地址 / 安全), the date
 * (relative + absolute tooltip), star, reply and the more menu.
 */
import type { ReactNode } from 'react';
import type { Address, Message } from '@/api/types';
import { resolveApiUrl } from '@/config';
import { Avatar, DropdownMenu, IconButton, Popover, Tooltip, type MenuEntry } from '@/components/common';
import { t } from '@/i18n/zh';
import { formatFullDate, formatMessageDate, formatRelative } from '@/lib/format';
import { displaySubject } from '@/lib/subject';
import { authSummary, formatAddress, recipientSummary } from './addressing';
import type { ReplyKind } from './compose';
import { DeliveryStatus } from './DeliveryStatus';
import { addressName } from './participants';

export interface MessageHeaderProps {
  message: Message;
  mine: ReadonlySet<string>;
  now: number;
  tz: string;
  /** Show 回复全部 (more than one other person involved). */
  replyAll: boolean;
  /** 删除此邮件 is possible only when the message is the thread's only one (no per-message trash API). */
  canDelete: boolean;
  onCollapse: () => void;
  onReply: (kind: ReplyKind) => void;
  onStar: () => void;
  onDelete: () => void;
  onMarkUnreadFromHere: () => void;
}

function DetailRow({ label, children }: { label: string; children: ReactNode }) {
  return (
    <>
      <dt className="whitespace-nowrap text-right text-on-surface-variant">{label}</dt>
      <dd className="min-w-0 break-words text-on-surface">{children}</dd>
    </>
  );
}

const list = (as: Address[]) => as.map(formatAddress).join('，');

export function MessageDetails({ message, tz }: { message: Message; tz: string }) {
  const auth = authSummary(message.auth);
  return (
    <dl className="grid grid-cols-[auto_minmax(0,1fr)] gap-x-3 gap-y-1.5 text-[13px]">
      <DetailRow label={t('mail.thread.details.from')}>{formatAddress(message.from)}</DetailRow>
      {message.sent_by && <DetailRow label={t('mail.thread.details.sentBy')}>{formatAddress(message.sent_by)}</DetailRow>}
      {message.reply_to.length > 0 && <DetailRow label={t('mail.thread.details.replyTo')}>{list(message.reply_to)}</DetailRow>}
      {message.to.length > 0 && <DetailRow label={t('mail.thread.details.to')}>{list(message.to)}</DetailRow>}
      {message.cc.length > 0 && <DetailRow label={t('mail.thread.details.cc')}>{list(message.cc)}</DetailRow>}
      {message.bcc.length > 0 && <DetailRow label={t('mail.thread.details.bcc')}>{list(message.bcc)}</DetailRow>}
      <DetailRow label={t('mail.thread.details.date')}>{formatFullDate(message.date, tz)}</DetailRow>
      <DetailRow label={t('mail.thread.details.subject')}>{displaySubject(message.subject)}</DetailRow>
      {message.delivered_to && <DetailRow label={t('mail.thread.details.deliveredTo')}>{message.delivered_to}</DetailRow>}
      {auth && <DetailRow label={t('mail.thread.details.security')}>{auth}</DetailRow>}
    </dl>
  );
}

export function MessageHeader(props: MessageHeaderProps) {
  const { message: m, mine, now, tz } = props;
  const name = addressName(m.from);
  const showEmail = name.toLowerCase() !== m.from.email.toLowerCase();
  const rel = formatRelative(m.date, now);
  const date = formatMessageDate(m.date, now, tz);
  const summary = recipientSummary(m, mine);

  const menu: MenuEntry[] = [
    { key: 'reply', label: t('mail.thread.menu.reply'), icon: 'reply', onSelect: () => props.onReply('reply'), shortcut: 'r' },
    ...(props.replyAll
      ? [{ key: 'reply_all', label: t('mail.thread.menu.replyAll'), icon: 'reply_all', onSelect: () => props.onReply('reply_all'), shortcut: 'a' }]
      : []),
    { key: 'forward', label: t('mail.thread.menu.forward'), icon: 'forward', onSelect: () => props.onReply('forward'), shortcut: 'f' },
    { type: 'separator', key: 's1' },
    ...(m.raw_url
      ? [
          {
            key: 'raw',
            label: t('mail.thread.menu.showOriginal'),
            icon: 'code',
            onSelect: () => window.open(resolveApiUrl(m.raw_url!), '_blank', 'noopener,noreferrer'),
          },
        ]
      : []),
    {
      key: 'delete',
      label: props.canDelete ? t('mail.thread.menu.deleteMessage') : `${t('mail.thread.menu.deleteMessage')}（${t('mail.thread.menu.deleteMessageUnavailable')}）`,
      icon: 'delete',
      disabled: !props.canDelete,
      onSelect: props.onDelete,
    },
    { key: 'unread', label: t('mail.thread.menu.markUnreadFromHere'), icon: 'mark_email_unread', onSelect: props.onMarkUnreadFromHere },
  ];

  return (
    <header className="flex gap-3">
      <Avatar name={m.from.name} email={m.from.email} size={40} decorative className="mt-0.5" />
      <div className="min-w-0 flex-1">
        <div className="flex items-start gap-2">
          {/* Clicking the sender line collapses the message (Gmail). */}
          <button
            type="button"
            aria-expanded
            onClick={props.onCollapse}
            className="min-w-0 flex-1 cursor-pointer rounded pt-0.5 text-left"
          >
            <span className="text-sm font-bold text-on-surface">{name}</span>
            {showEmail && <span className="ml-1 break-all text-xs text-on-surface-variant">&lt;{m.from.email}&gt;</span>}
            {m.sent_by && <span className="ml-2 text-xs text-on-surface-variant">{t('mail.thread.sentBy', { name: addressName(m.sent_by) })}</span>}
          </button>
          <div className="-mr-2 -mt-1 flex shrink-0 items-center">
            <Tooltip label={formatFullDate(m.date, tz)}>
              <span className="mr-1 hidden whitespace-nowrap text-xs text-on-surface-variant sm:inline">
                {date}
                {rel && ` (${rel})`}
              </span>
            </Tooltip>
            <IconButton
              icon="star"
              fill={m.is_starred}
              label={m.is_starred ? t('mail.thread.unstarMessage') : t('mail.thread.starMessage')}
              size="sm"
              className={m.is_starred ? 'text-star hover:text-star' : undefined}
              onClick={props.onStar}
            />
            <IconButton icon="reply" label={t('mail.thread.reply')} size="sm" onClick={() => props.onReply('reply')} />
            <DropdownMenu
              align="end"
              items={menu}
              aria-label={t('mail.thread.menu.label')}
              trigger={<IconButton icon="more_vert" label={t('mail.thread.menu.label')} size="sm" />}
            />
          </div>
        </div>
        <div className="flex min-w-0 flex-wrap items-center gap-x-1 gap-y-1 text-xs text-on-surface-variant">
          <span className="min-w-0 max-w-full truncate">{t('mail.thread.sendTo', { names: summary || '-' })}</span>
          <Popover
            align="start"
            className="w-[min(480px,calc(100vw-32px))] p-4"
            aria-label={t('mail.thread.showDetails')}
            trigger={<IconButton icon="arrow_drop_down" label={t('mail.thread.showDetails')} size="sm" className="-my-1 size-6" />}
          >
            <MessageDetails message={m} tz={tz} />
          </Popover>
          <span className="whitespace-nowrap sm:hidden">
            · {date}
          </span>
          {m.direction === 'out' && m.outbound && (
            <span className="ml-1">
              <DeliveryStatus message={m} now={now} tz={tz} />
            </span>
          )}
        </div>
      </div>
    </header>
  );
}
