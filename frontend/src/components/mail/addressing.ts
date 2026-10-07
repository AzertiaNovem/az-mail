/**
 * Address display helpers for the message header [WP-E] ("发送至 我、张三", details popover).
 * Reply / reply-all recipient calculation lives in WP-F's lib/recipients.ts.
 */
import type { Address, Message } from '@/api/types';
import { t } from '@/i18n/zh';
import { addressName, shortName } from './participants';

export function isMine(email: string | null | undefined, mine: ReadonlySet<string>): boolean {
  return !!email && mine.has(email.trim().toLowerCase());
}

/** "张三 <zs@example.com>" (just the email when there is no name). */
export function formatAddress(a: Address): string {
  const name = (a.name ?? '').trim();
  return name && name.toLowerCase() !== a.email.toLowerCase() ? `${name} <${a.email}>` : a.email;
}

/** "发送至 …" summary: names of To + Cc (+ Bcc on our own copies); "我" for my addresses. */
export function recipientSummary(m: Pick<Message, 'to' | 'cc' | 'bcc' | 'direction'>, mine: ReadonlySet<string>): string {
  const list = [...m.to, ...m.cc, ...(m.direction === 'out' ? m.bcc : [])];
  const seen = new Set<string>();
  const names: string[] = [];
  for (const a of list) {
    const key = a.email.toLowerCase();
    if (seen.has(key)) continue;
    seen.add(key);
    names.push(isMine(a.email, mine) ? t('mail.thread.toMe') : list.length > 1 ? shortName(a) : addressName(a));
  }
  // An inbound copy where we are only in Bcc (C2: stored as bcc:[self]).
  if (m.direction === 'in' && !list.some((a) => isMine(a.email, mine)) && m.bcc.some((a) => isMine(a.email, mine))) {
    return names.length ? `${names.join('、')}、${t('mail.thread.toMeBcc')}` : t('mail.thread.toMeBcc');
  }
  return names.join('、');
}

/** Distinct people a reply-all would reach (sender + To + Cc, minus me). */
export function replyAllCount(m: Pick<Message, 'from' | 'to' | 'cc' | 'reply_to'>, mine: ReadonlySet<string>): number {
  const set = new Set<string>();
  for (const a of [...(m.reply_to.length ? m.reply_to : [m.from]), ...m.to, ...m.cc]) {
    if (!isMine(a.email, mine)) set.add(a.email.toLowerCase());
  }
  return set.size;
}

function authValue(v: string | null): string {
  if (v === null || v === '') return t('mail.thread.auth.none');
  const s = v.toLowerCase();
  if (s === 'pass') return t('mail.thread.auth.pass');
  if (s === 'fail' || s === 'softfail' || s === 'permerror' || s === 'temperror') return t('mail.thread.auth.fail');
  return v;
}

/** "SPF 通过 · DKIM 通过 · DMARC 未通过", or null when the message has no auth results. */
export function authSummary(auth: Message['auth']): string | null {
  if (!auth) return null;
  return t('mail.thread.auth.line', { spf: authValue(auth.spf), dkim: authValue(auth.dkim), dmarc: authValue(auth.dmarc) });
}
