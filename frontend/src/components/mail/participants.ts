/**
 * Thread-row sender column (Gmail style) [WP-E]: "张三, 我 3", "Alice .. Bob, 我 7",
 * "我, 草稿 2", and "草稿" alone in the drafts folder. Unread participants are bold.
 */
import type { Address, FolderId, ThreadListItem } from '@/api/types';
import { t } from '@/i18n/zh';

export interface ParticipantPart {
  text: string;
  bold: boolean;
  /** Red "草稿" marker. */
  draft?: boolean;
  /** The ".." between first and last participants. */
  ellipsis?: boolean;
}

export interface ParticipantsView {
  parts: ParticipantPart[];
  /** Message count shown after the names (null when 1). */
  count: number | null;
  /** Full list for the tooltip. */
  title: string;
}

/** Display name of an address: the name, else the local part of the email. */
export function addressName(a: Pick<Address, 'name' | 'email'>): string {
  const name = (a.name ?? '').trim().replace(/^["']+|["']+$/g, '');
  if (name) return name;
  const email = (a.email ?? '').trim();
  return email.includes('@') ? email.slice(0, email.indexOf('@')) : email;
}

/** Shorter form used when several names share the column: first word of Latin names. */
export function shortName(a: Pick<Address, 'name' | 'email'>): string {
  const full = addressName(a);
  if (/^[\x20-\x7e]+$/.test(full) && full.includes(' ')) return full.split(/\s+/)[0] ?? full;
  return full;
}

export function participantsView(item: ThreadListItem, folder: FolderId | null): ParticipantsView {
  const draftOnly = folder === 'drafts' || (item.message_count === 0 && item.draft_count > 0);
  const draftLabel = t('mail.list.draft');
  if (draftOnly) {
    return {
      parts: [{ text: draftLabel, bold: false, draft: true }],
      count: item.draft_count > 1 ? item.draft_count : null,
      title: draftLabel,
    };
  }
  const people = item.participants;
  const many = people.length > 1;
  const named = people.map<ParticipantPart>((p) => ({
    text: p.is_me ? t('mail.list.me') : many ? shortName(p) : addressName(p),
    bold: item.unread && p.unread,
  }));
  let parts: ParticipantPart[] = named;
  if (named.length > 3) {
    parts = [named[0]!, { text: '..', bold: false, ellipsis: true }, ...named.slice(-2)];
  }
  if (parts.length === 0) parts = [{ text: t('mail.list.me'), bold: false }];
  if (item.draft_count > 0) parts = [...parts, { text: draftLabel, bold: false, draft: true }];
  const total = item.message_count + item.draft_count;
  return {
    parts,
    count: total > 1 ? total : null,
    title: people.map((p) => (p.is_me ? t('mail.list.me') : `${addressName(p)} <${p.email}>`)).join(', '),
  };
}
