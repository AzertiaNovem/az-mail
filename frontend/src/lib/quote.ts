/**
 * Quote builders for replies and forwards [WP-F] (DESIGN.md §1 C10, §5).
 *
 * The quoted original is kept OUTSIDE the editor in `quoted_html` (ProseMirror would mangle
 * arbitrary email HTML) and appended by the server when the send is frozen. It is built from the
 * cached parent Message: its HTML (signed /api/files URLs + data-att-id, which the server maps
 * back to cid:) is sanitized with `sanitizeEmailHtml` [E]; `<style>` / `<title>` blocks are
 * dropped so a quoted newsletter cannot restyle the reply. Plain-text parents are escaped.
 *
 *   reply:    在 2026年10月7日周三 20:03，张三 <a@b> 写道：  + <blockquote>
 *   forward:  ---------- 转发的邮件 ---------
 *             发件人：/ 日期：/ 主题：/ 收件人：/ 抄送：      + original body
 *
 * Dates are rendered in the user's display timezone (Settings.timezone, default Asia/Shanghai).
 */
import type { Address, Message } from '@/api/types';
import { t } from '@/i18n/zh';
import { displayAddress } from './recipients';
import { sanitizeEmailHtml } from './sanitize';

export const DEFAULT_TIMEZONE = 'Asia/Shanghai';

// ───────────── time zones ─────────────

const validZones = new Map<string, boolean>();

/** `tz` when the runtime knows it, else Asia/Shanghai. */
export function safeTimeZone(tz: string | null | undefined): string {
  if (!tz) return DEFAULT_TIMEZONE;
  let ok = validZones.get(tz);
  if (ok === undefined) {
    try {
      new Intl.DateTimeFormat('en-US', { timeZone: tz });
      ok = true;
    } catch {
      ok = false;
    }
    validZones.set(tz, ok);
  }
  return ok ? tz : DEFAULT_TIMEZONE;
}

export interface ZonedParts {
  year: number;
  /** 1–12 */
  month: number;
  day: number;
  hour: number;
  minute: number;
  second: number;
  /** 0 = Sunday … 6 = Saturday */
  weekday: number;
}

const partFormatters = new Map<string, Intl.DateTimeFormat>();
const WEEKDAY_INDEX: Record<string, number> = { Sun: 0, Mon: 1, Tue: 2, Wed: 3, Thu: 4, Fri: 5, Sat: 6 };

/** Wall-clock fields of `ms` in `tz`. */
export function zonedParts(ms: number, tz: string): ZonedParts {
  const zone = safeTimeZone(tz);
  let fmt = partFormatters.get(zone);
  if (!fmt) {
    fmt = new Intl.DateTimeFormat('en-US', {
      timeZone: zone,
      hourCycle: 'h23',
      year: 'numeric',
      month: 'numeric',
      day: 'numeric',
      hour: 'numeric',
      minute: 'numeric',
      second: 'numeric',
      weekday: 'short',
    });
    partFormatters.set(zone, fmt);
  }
  const out: ZonedParts = { year: 0, month: 0, day: 0, hour: 0, minute: 0, second: 0, weekday: 0 };
  for (const p of fmt.formatToParts(new Date(ms))) {
    const n = Number(p.value);
    switch (p.type) {
      case 'year':
        out.year = n;
        break;
      case 'month':
        out.month = n;
        break;
      case 'day':
        out.day = n;
        break;
      case 'hour':
        out.hour = n % 24; // some ICU builds still print "24" at midnight
        break;
      case 'minute':
        out.minute = n;
        break;
      case 'second':
        out.second = n;
        break;
      case 'weekday':
        out.weekday = WEEKDAY_INDEX[p.value] ?? 0;
        break;
      default:
        break;
    }
  }
  return out;
}

const ZH_WEEKDAYS = ['周日', '周一', '周二', '周三', '周四', '周五', '周六'] as const;
const pad2 = (n: number) => String(n).padStart(2, '0');

export const zhWeekday = (weekday: number): string => ZH_WEEKDAYS[weekday] ?? '';

/** "2026年10月7日周三 20:03" (year omitted with `omitYear`). */
export function formatZhDateTime(ms: number, tz: string, opts: { omitYear?: boolean } = {}): string {
  const p = zonedParts(ms, tz);
  const date = `${opts.omitYear ? '' : `${p.year}年`}${p.month}月${p.day}日${zhWeekday(p.weekday)}`;
  return `${date} ${pad2(p.hour)}:${pad2(p.minute)}`;
}

/** Date used in quote headers: "2026年10月7日周三 20:03". */
export const formatQuoteDate = (ms: number, tz: string): string => formatZhDateTime(ms, tz);

// ───────────── HTML helpers ─────────────

export function escapeHtml(s: string): string {
  return s
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;')
    .replace(/'/g, '&#39;');
}

/** Escaped plain text with line breaks preserved. */
export function textToHtml(text: string): string {
  return escapeHtml(text.replace(/\r\n?/g, '\n')).replace(/\n/g, '<br>');
}

/** Sanitized parent body for embedding in a quote (no <style>/<title>, links open in a new tab). */
export function sanitizeQuotedBody(html: string, filesOrigins: string[]): string {
  const { html: clean } = sanitizeEmailHtml(html, { allowRemote: true, filesOrigins });
  const doc = new DOMParser().parseFromString(`<!doctype html><body>${clean}</body>`, 'text/html');
  for (const el of Array.from(doc.body.querySelectorAll('style, title'))) el.remove();
  return doc.body.innerHTML.trim();
}

function parentBodyHtml(parent: Message, filesOrigins: string[]): string {
  if (parent.html && parent.html.trim()) return sanitizeQuotedBody(parent.html, filesOrigins);
  return textToHtml(parent.text ?? '');
}

const addressHtml = (a: Address): string => escapeHtml(displayAddress(a));
const addressListHtml = (list: readonly Address[]): string => list.map(addressHtml).join(', ');

// ───────────── builders ─────────────

export interface QuoteOptions {
  /** Display timezone (Settings.timezone). */
  timeZone: string;
  /** `Me.server.files_origins`, passed through to the sanitizer. */
  filesOrigins: string[];
}

/** Plain-text attribution line: "在 2026年10月7日周三 20:03，张三 <a@b> 写道：". */
export function quoteAttribution(parent: Pick<Message, 'date' | 'from'>, tz: string): string {
  return t('compose.quote.attribution', { date: formatQuoteDate(parent.date, tz), sender: displayAddress(parent.from) });
}

/** quoted_html of a reply / reply-all. */
export function buildReplyQuote(parent: Message, opts: QuoteOptions): string {
  const attribution = escapeHtml(quoteAttribution(parent, opts.timeZone));
  return (
    `<div class="gmail_quote azm_quote">` +
    `<div dir="ltr" class="gmail_attr">${attribution}<br></div>` +
    `<blockquote class="gmail_quote" style="margin:0px 0px 0px 0.8ex;border-left:1px solid rgb(204,204,204);padding-left:1ex">` +
    parentBodyHtml(parent, opts.filesOrigins) +
    `</blockquote></div>`
  );
}

/** quoted_html of a forward: the "转发的邮件" header block followed by the original body. */
export function buildForwardQuote(parent: Message, opts: QuoteOptions): string {
  const lines = [
    escapeHtml(t('compose.quote.forwardHeader')),
    `${escapeHtml(t('compose.quote.from'))}${
      parent.from.name.trim()
        ? `<strong class="gmail_sendername" dir="auto">${escapeHtml(parent.from.name.trim())}</strong> <span dir="auto">&lt;${escapeHtml(parent.from.email)}&gt;</span>`
        : `&lt;${escapeHtml(parent.from.email)}&gt;`
    }`,
    `${escapeHtml(t('compose.quote.date'))}${escapeHtml(formatQuoteDate(parent.date, opts.timeZone))}`,
    `${escapeHtml(t('compose.quote.subject'))}${escapeHtml(parent.subject)}`,
    `${escapeHtml(t('compose.quote.to'))}${addressListHtml(parent.to)}`,
  ];
  if (parent.cc.length) lines.push(`${escapeHtml(t('compose.quote.cc'))}${addressListHtml(parent.cc)}`);
  return (
    `<div class="gmail_quote azm_quote">` +
    `<div dir="ltr" class="gmail_attr">${lines.join('<br>')}<br></div><br><br>` +
    parentBodyHtml(parent, opts.filesOrigins) +
    `</div>`
  );
}

/** quoted_html for a compose mode (null for a new message). */
export function buildQuotedHtml(mode: 'new' | 'reply' | 'reply_all' | 'forward', parent: Message | null, opts: QuoteOptions): string | null {
  if (!parent || mode === 'new') return null;
  return mode === 'forward' ? buildForwardQuote(parent, opts) : buildReplyQuote(parent, opts);
}
