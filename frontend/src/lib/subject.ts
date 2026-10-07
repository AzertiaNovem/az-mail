/**
 * Subject helpers [WP-E]. Mirrors the server's reply-prefix rule (DESIGN §1 C7): Re / Fw / Fwd /
 * 回复 / 答复 / 转发 / 回覆 / 轉寄, followed by ":", "：" or a "[n]" counter (e.g. "Re[2]:").
 */
import { t } from '@/i18n/zh';

/** One leading reply/forward prefix (case-insensitive), with optional "[n]" and the colon. */
const PREFIX_RE = /^\s*(?:re|fwd?|回复|答复|转发|回覆|轉寄)\s*(?:\[\d+\]\s*)?[:：]\s*/i;
/** "Re[2] foo" (counter without colon) also counts as a prefix. */
const COUNTER_PREFIX_RE = /^\s*(?:re|fwd?|回复|答复|转发|回覆|轉寄)\s*\[\d+\]\s*/i;

/** Whether the subject starts with a reply/forward prefix. */
export function hasReplyPrefix(subject: string): boolean {
  return PREFIX_RE.test(subject) || COUNTER_PREFIX_RE.test(subject);
}

/** Removes every leading reply/forward prefix: "Re: 回复：Fwd: 周报" → "周报". */
export function stripReplyPrefixes(subject: string): string {
  let s = subject;
  for (let i = 0; i < 20; i++) {
    const next = s.replace(PREFIX_RE, '').replace(COUNTER_PREFIX_RE, '');
    if (next === s) break;
    s = next;
  }
  return s.trim();
}

/** Lower-cased, prefix-free, whitespace-collapsed subject for comparisons. */
export function normalizeSubject(subject: string): string {
  return stripReplyPrefixes(subject).replace(/\s+/g, ' ').toLowerCase();
}

/** Subject for display: trimmed, or "(无主题)" when empty. */
export function displaySubject(subject: string | null | undefined): string {
  const s = (subject ?? '').replace(/\s+/g, ' ').trim();
  return s || t('common.noSubject');
}

/** Whether a subject is empty after trimming. */
export function isEmptySubject(subject: string | null | undefined): boolean {
  return !(subject ?? '').trim();
}
