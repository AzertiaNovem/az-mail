/**
 * Email iframe document [WP-E] — DESIGN.md §5 steps 4–5 and 9, §1 D3.
 *
 * The sanitized body goes into `<iframe sandbox="allow-same-origin allow-popups
 * allow-popups-to-escape-sandbox" referrerpolicy="no-referrer" srcdoc=…>`: no `allow-scripts`,
 * so nothing in the mail can run; same-origin only lets the parent measure the height and
 * intercept mailto: clicks. The srcdoc carries its own meta CSP, which does the real
 * restricting (srcdoc frames also inherit the app CSP, which allows `img-src https:`):
 *
 *   default-src 'none'; img-src data: <files origins> [https: http: when remote images are
 *   allowed]; style-src 'unsafe-inline'; font-src data:; form-action 'none'
 *
 * plus `<base target="_blank">`, a no-referrer policy and light base styles (emails always
 * render on a white background, whatever the app theme).
 */
import type { Address } from '@/api/types';
import { apiUrl, config, type RuntimeConfig } from '@/config';
import { normalizeOrigins, QUOTE_ATTR } from './sanitize';

export const EMAIL_FRAME_SANDBOX = 'allow-same-origin allow-popups allow-popups-to-escape-sandbox';

/** Origin serving the API for this page (resolves a relative / empty apiBase against the page). */
export function apiOrigin(cfg: RuntimeConfig = config): string | null {
  try {
    const base = typeof window !== 'undefined' ? window.location.href : 'http://localhost/';
    return new URL(apiUrl('/', cfg), base).origin;
  } catch {
    return null;
  }
}

/**
 * Origins whose images always load in the frame: `Me.server.files_origins` plus the API origin
 * as this page reaches it (covers a same-origin / proxied API in dev). Normalized and deduped.
 */
export function frameFilesOrigins(filesOrigins: readonly string[], cfg: RuntimeConfig = config): string[] {
  const own = apiOrigin(cfg);
  return [...normalizeOrigins(own ? [...filesOrigins, own] : filesOrigins)];
}

export interface FrameDocOptions {
  allowRemote: boolean;
  /** Already-normalized origins (see frameFilesOrigins). Anything that is not a plain origin is dropped. */
  filesOrigins: readonly string[];
  /** Hide the block marked by `sanitizeEmailHtml({markQuotes:true})`. */
  collapseQuotes?: boolean;
}

/** The meta CSP of the email document. */
export function buildFrameCsp(opts: Pick<FrameDocOptions, 'allowRemote' | 'filesOrigins'>): string {
  // Only plain scheme://host[:port] origins may enter the policy (no spaces, quotes or ';').
  const origins = [...normalizeOrigins(opts.filesOrigins)];
  const img = ['data:', ...origins, ...(opts.allowRemote ? ['https:', 'http:'] : [])].join(' ');
  return `default-src 'none'; img-src ${img}; style-src 'unsafe-inline'; font-src data:; form-action 'none'`;
}

const escapeAttr = (s: string) => s.replace(/&/g, '&amp;').replace(/"/g, '&quot;').replace(/</g, '&lt;');

/** Base styles; the email's own styles come later and win. */
const BASE_CSS = [
  ':root{color-scheme:light}',
  'html{background:#fff;overflow-x:auto;overflow-y:hidden}',
  'html,body{margin:0;padding:0}',
  'body{font-family:Arial,"PingFang SC","Microsoft YaHei","Noto Sans SC",sans-serif;font-size:14px;line-height:1.5;color:#222;',
  'overflow-wrap:break-word;word-wrap:break-word;-webkit-text-size-adjust:100%;text-size-adjust:100%}',
  'img{max-width:100%}',
  'pre{white-space:pre-wrap}',
  'blockquote[type=cite]{margin:0 0 0 .8ex;border-left:1px solid #ccc;padding-left:1ex}',
  'a{color:#0b57d0}',
].join('');

/** The complete srcdoc for a sanitized body fragment. */
export function buildSrcDoc(bodyHtml: string, opts: FrameDocOptions): string {
  const csp = buildFrameCsp(opts);
  const collapse = opts.collapseQuotes ? `[${QUOTE_ATTR}]{display:none!important}` : '';
  return (
    '<!DOCTYPE html><html><head><meta charset="utf-8">' +
    `<meta http-equiv="Content-Security-Policy" content="${escapeAttr(csp)}">` +
    '<meta name="referrer" content="no-referrer">' +
    '<base target="_blank">' +
    `<style>${BASE_CSS}${collapse}</style>` +
    `</head><body>${bodyHtml}</body></html>`
  );
}

/** Height the frame needs to show `doc` without a vertical scrollbar (px). */
export function measureContentHeight(doc: Document): number {
  const html = doc.documentElement;
  const body = doc.body;
  if (!html) return 0;
  let h = html.getBoundingClientRect().height;
  if (body) {
    const cs = doc.defaultView?.getComputedStyle(body);
    const margins = cs ? (Number.parseFloat(cs.marginTop) || 0) + (Number.parseFloat(cs.marginBottom) || 0) : 0;
    h = Math.max(h, body.scrollHeight + margins);
  }
  // A horizontal scrollbar (wide fixed-width layouts) takes height from the viewport.
  if (html.scrollWidth > html.clientWidth + 1) h += 17;
  return Math.ceil(h);
}

// ───────────── mailto: ─────────────

export interface MailtoParts {
  to: Address[];
  cc: Address[];
  bcc: Address[];
  subject: string;
  body: string;
}

const EMAIL_RE = /^[^\s@<>(),;:"]+@[^\s@<>(),;:"]+$/;

/** RFC 6068: percent-decoding only ('+' is literal, e.g. user+tag@example.com). */
function decode(s: string): string {
  try {
    return decodeURIComponent(s);
  } catch {
    return s;
  }
}

function addresses(list: string): Address[] {
  return list
    .split(/[,;]/)
    .map((s) => decode(s).trim())
    .filter((s) => EMAIL_RE.test(s))
    .map((email) => ({ name: '', email }));
}

/** Parses `mailto:a@b.com,c@d.com?subject=…&cc=…&body=…`; null when it is not a mailto: URL. */
export function parseMailto(href: string): MailtoParts | null {
  const m = /^\s*mailto:([^?#]*)(?:\?([^#]*))?/i.exec(href);
  if (!m) return null;
  const out: MailtoParts = { to: addresses(m[1] ?? ''), cc: [], bcc: [], subject: '', body: '' };
  for (const pair of (m[2] ?? '').split('&')) {
    if (!pair) continue;
    const eq = pair.indexOf('=');
    const key = decode(eq === -1 ? pair : pair.slice(0, eq)).toLowerCase();
    const value = eq === -1 ? '' : pair.slice(eq + 1);
    if (key === 'to') out.to.push(...addresses(value));
    else if (key === 'cc') out.cc.push(...addresses(value));
    else if (key === 'bcc') out.bcc.push(...addresses(value));
    else if (key === 'subject') out.subject = decode(value);
    else if (key === 'body') out.body = decode(value);
  }
  return out;
}
