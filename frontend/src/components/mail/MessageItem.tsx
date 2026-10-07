/**
 * One message in the thread view [WP-E]: a one-line collapsed row (avatar, sender, snippet,
 * date), a red "草稿" row for drafts (opens compose), or the expanded message (header,
 * warnings, spam notice, remote-image banner, sandboxed HTML or plain text, attachments).
 */
import type { KeyboardEvent } from 'react';
import type { Attachment, Message } from '@/api/types';
import { Avatar, Button, cx, Icon } from '@/components/common';
import { t } from '@/i18n/zh';
import type { MailtoParts } from '@/lib/emailFrame';
import { formatFullDate, formatMessageDate } from '@/lib/format';
import { displaySubject } from '@/lib/subject';
import { AttachmentList, listedAttachments } from './AttachmentList';
import type { ReplyKind } from './compose';
import { EmailFrame, useEmailContent } from './EmailFrame';
import { MessageHeader } from './MessageHeader';
import { addressName } from './participants';
import { PlainTextBody } from './PlainTextBody';
import { RemoteImagesBanner } from './RemoteImagesBanner';

export interface MessageItemProps {
  message: Message;
  expanded: boolean;
  mine: ReadonlySet<string>;
  now: number;
  tz: string;
  filesOrigins: readonly string[];
  /** Remote images may load (setting, trusted sender, or "显示图片" clicked). */
  allowRemote: boolean;
  /** "始终显示来自 x" may be offered (not for spam / suspicious mail). */
  canTrustSender: boolean;
  trustBusy?: boolean;
  replyAll: boolean;
  canDelete: boolean;
  onToggle: () => void;
  onOpenDraft: () => void;
  onReply: (kind: ReplyKind) => void;
  onStar: () => void;
  onDelete: () => void;
  onMarkUnreadFromHere: () => void;
  onShowImages: () => void;
  onTrustSender: () => void;
  onNotSpam: () => void;
  onPreview: (a: Attachment) => void;
  onMailto: (parts: MailtoParts) => void;
}

const activate = (fn: () => void) => (e: KeyboardEvent) => {
  if (e.key === 'Enter' || e.key === ' ') {
    e.preventDefault();
    fn();
  }
};

function Warnings({ message }: { message: Message }) {
  if (!message.warnings.length) return null;
  return (
    <div className="mb-3 flex flex-col gap-2">
      {message.warnings.map((w) => (
        <div key={w} role="alert" className="flex items-start gap-2 rounded-lg bg-error-container px-3 py-2.5 text-[13px] text-on-error-container">
          <Icon name="gpp_maybe" size={20} className="mt-px text-error" />
          <span>{t(`mail.thread.warnings.${w}`)}</span>
        </div>
      ))}
    </div>
  );
}

function Body({ props }: { props: MessageItemProps }) {
  const m = props.message;
  const content = useEmailContent(m.html ?? '', props.allowRemote, props.filesOrigins);
  const blocked = !props.allowRemote && content.result.blockedImages > 0;
  return (
    <>
      {blocked && (
        <RemoteImagesBanner
          sender={m.from.email}
          canTrust={props.canTrustSender}
          busy={props.trustBusy}
          onShow={props.onShowImages}
          onAlways={props.onTrustSender}
        />
      )}
      {m.html ? (
        <EmailFrame content={content} title={t('mail.thread.frameTitle', { subject: displaySubject(m.subject) })} onMailto={props.onMailto} />
      ) : m.text ? (
        <PlainTextBody text={m.text} onEmailClick={(email) => props.onMailto({ to: [{ name: '', email }], cc: [], bcc: [], subject: '', body: '' })} />
      ) : (
        <p className="text-sm italic text-on-surface-variant">{t('mail.thread.emptyBody')}</p>
      )}
    </>
  );
}

export function MessageItem(props: MessageItemProps) {
  const { message: m, expanded, now, tz } = props;
  const name = addressName(m.from);
  const unread = !m.is_read && !m.is_draft;

  if (m.is_draft) {
    return (
      <div
        role="button"
        tabIndex={0}
        onClick={props.onOpenDraft}
        onKeyDown={activate(props.onOpenDraft)}
        aria-label={`${t('mail.thread.draft')}：${m.snippet || displaySubject(m.subject)}，${t('mail.thread.openDraft')}`}
        className="flex cursor-pointer items-center gap-3 border-b border-divider px-4 py-3 hover:bg-hover sm:px-6"
      >
        <Avatar name={m.from.name} email={m.from.email} size={40} decorative />
        <span className="shrink-0 text-sm text-[#d93025]">{t('mail.thread.draft')}</span>
        <span className="min-w-0 flex-1 truncate text-sm text-on-surface-variant">{m.snippet || displaySubject(m.subject)}</span>
        <Icon name="edit" size={18} className="text-on-surface-variant" />
        <span className="shrink-0 text-xs text-on-surface-variant" title={formatFullDate(m.date, tz)}>
          {formatMessageDate(m.date, now, tz)}
        </span>
      </div>
    );
  }

  if (!expanded) {
    return (
      <div
        role="button"
        tabIndex={0}
        aria-expanded={false}
        onClick={props.onToggle}
        onKeyDown={activate(props.onToggle)}
        className="flex cursor-pointer items-center gap-3 border-b border-divider px-4 py-3 hover:bg-hover sm:px-6"
      >
        <Avatar name={m.from.name} email={m.from.email} size={40} decorative />
        <span className={cx('min-w-0 flex-1 truncate text-sm sm:w-[180px] sm:flex-none sm:shrink-0', unread ? 'font-bold text-on-surface' : 'text-on-surface')}>{name}</span>
        <span className="hidden min-w-0 flex-1 truncate text-sm text-on-surface-variant sm:block">{m.snippet}</span>
        {m.attachments.length > 0 && <Icon name="attach_file" size={16} className="text-on-surface-variant" />}
        {m.is_starred && <Icon name="star" size={16} fill className="text-star" />}
        <span className={cx('shrink-0 text-xs', unread ? 'font-bold text-on-surface' : 'text-on-surface-variant')} title={formatFullDate(m.date, tz)}>
          {formatMessageDate(m.date, now, tz)}
        </span>
      </div>
    );
  }

  const attachments = listedAttachments(m);
  return (
    <article className="border-b border-divider px-4 pb-5 pt-4 sm:px-6" aria-label={`${name}：${displaySubject(m.subject)}`}>
      <MessageHeader
        message={m}
        mine={props.mine}
        now={now}
        tz={tz}
        replyAll={props.replyAll}
        canDelete={props.canDelete}
        onCollapse={props.onToggle}
        onReply={props.onReply}
        onStar={props.onStar}
        onDelete={props.onDelete}
        onMarkUnreadFromHere={props.onMarkUnreadFromHere}
      />
      <div className="mt-4 sm:pl-[52px]">
        <Warnings message={m} />
        {m.is_spam && (
          <div className="mb-3 flex flex-wrap items-center gap-3 rounded-lg bg-warning-container px-3 py-2 text-[13px] text-on-surface">
            <Icon name="report" size={20} className="text-warning" />
            <span className="flex-1">{t('mail.thread.spamBanner')}</span>
            <Button variant="outlined" size="sm" onClick={props.onNotSpam}>
              {t('mail.thread.spamBannerAction')}
            </Button>
          </div>
        )}
        <Body props={props} />
        <AttachmentList attachments={attachments} onPreview={props.onPreview} />
      </div>
    </article>
  );
}
