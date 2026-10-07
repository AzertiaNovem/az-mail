/**
 * Plain-text message body [WP-E] (DESIGN.md §5 step 7): escaped (rendered as React text, never
 * as HTML), URLs and email addresses linkified, `pre-wrap` whitespace, and a trailing quoted
 * block ("> …" lines with their "… 写道：" attribution) collapsed behind "…".
 */
import { useMemo, useState, type ReactNode } from 'react';
import { Icon, Tooltip } from '@/components/common';
import { t } from '@/i18n/zh';

export type TextSegment = { kind: 'text'; value: string } | { kind: 'url'; value: string; href: string } | { kind: 'email'; value: string };

// URLs stop at whitespace, quotes, angle brackets and CJK punctuation.
const LINK_RE =
  /((?:https?:\/\/|www\.)[^\s<>"'，。！？、；：（）【】《》「」]+)|([A-Za-z0-9._%+-]+@[A-Za-z0-9-]+(?:\.[A-Za-z0-9-]+)*\.[A-Za-z]{2,})/gi;
const TRAILING_PUNCT = /[.,;:!?)\]}'"]+$/;

/** Splits text into plain / URL / email segments. */
export function linkify(text: string): TextSegment[] {
  const out: TextSegment[] = [];
  let last = 0;
  for (const m of text.matchAll(LINK_RE)) {
    const start = m.index ?? 0;
    let value = m[0];
    if (m[1]) {
      // Keep a closing ')' that belongs to the URL ("…/wiki/Foo_(bar)").
      const trail = TRAILING_PUNCT.exec(value)?.[0] ?? '';
      let cut = trail.length;
      if (trail.startsWith(')') && (value.match(/\(/g)?.length ?? 0) >= (value.match(/\)/g)?.length ?? 0)) cut = 0;
      if (cut) value = value.slice(0, -cut);
    }
    if (start > last) out.push({ kind: 'text', value: text.slice(last, start) });
    if (m[1]) out.push({ kind: 'url', value, href: /^https?:\/\//i.test(value) ? value : `https://${value}` });
    else out.push({ kind: 'email', value });
    last = start + value.length;
  }
  if (last < text.length) out.push({ kind: 'text', value: text.slice(last) });
  return out;
}

const ATTRIBUTION_RE = /(?:wrote|写道|寫道)\s*[:：]?\s*$/i;
const ORIGINAL_RE = /^\s*-{2,}\s*(?:original message|原始邮件|原始郵件)\s*-{2,}\s*$/i;

/** Separates a trailing quoted block. `quoted` is null when there is none (or nothing else). */
export function splitQuoted(text: string): { main: string; quoted: string | null } {
  const lines = text.replace(/\r\n?/g, '\n').split('\n');
  // Outlook-style "-----原始邮件-----": everything from there on is the quote.
  const orig = lines.findIndex((l) => ORIGINAL_RE.test(l));
  let start = orig > 0 ? orig : -1;
  if (start === -1) {
    let i = lines.length - 1;
    while (i >= 0 && !lines[i]!.trim()) i--;
    let k = i;
    while (k >= 0 && (lines[k]!.trimStart().startsWith('>') || !lines[k]!.trim())) k--;
    // k is the last line that is not part of the "> " block.
    if (k < i && lines[i]!.trimStart().startsWith('>')) {
      start = k + 1;
      let j = k;
      while (j >= 0 && !lines[j]!.trim()) j--;
      if (j >= 0 && lines[j]!.length < 300 && ATTRIBUTION_RE.test(lines[j]!)) start = j;
    }
  }
  if (start <= 0) return { main: text, quoted: null };
  const main = lines.slice(0, start).join('\n').replace(/\s+$/, '');
  if (!main.trim()) return { main: text, quoted: null };
  return { main, quoted: lines.slice(start).join('\n') };
}

export interface PlainTextBodyProps {
  text: string;
  onEmailClick?: (email: string) => void;
}

function renderSegments(text: string, onEmailClick?: (email: string) => void): ReactNode[] {
  return linkify(text).map((s, i) => {
    if (s.kind === 'text') return s.value;
    if (s.kind === 'url')
      return (
        <a key={i} href={s.href} target="_blank" rel="noopener noreferrer nofollow" className="text-link hover:underline">
          {s.value}
        </a>
      );
    return (
      <a
        key={i}
        href={`mailto:${s.value}`}
        className="text-link hover:underline"
        onClick={(e) => {
          if (!onEmailClick) return;
          e.preventDefault();
          onEmailClick(s.value);
        }}
      >
        {s.value}
      </a>
    );
  });
}

export function PlainTextBody({ text, onEmailClick }: PlainTextBodyProps) {
  const { main, quoted } = useMemo(() => splitQuoted(text), [text]);
  const [showQuote, setShowQuote] = useState(false);
  return (
    <div className="azm-plain-text">
      <div className="whitespace-pre-wrap break-words">{renderSegments(main, onEmailClick)}</div>
      {quoted !== null && (
        <>
          <Tooltip label={showQuote ? t('mail.thread.hideQuoted') : t('mail.thread.showQuoted')}>
            <button
              type="button"
              aria-label={showQuote ? t('mail.thread.hideQuoted') : t('mail.thread.showQuoted')}
              aria-expanded={showQuote}
              onClick={() => setShowQuote((v) => !v)}
              className="azm-quote-toggle mt-2"
            >
              <Icon name="more_horiz" size={18} />
            </button>
          </Tooltip>
          {showQuote && <div className="mt-2 whitespace-pre-wrap break-words text-on-surface-variant">{renderSegments(quoted, onEmailClick)}</div>}
        </>
      )}
    </div>
  );
}
