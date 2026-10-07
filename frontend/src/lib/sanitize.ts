/**
 * Email HTML sanitizer [WP-E] — DESIGN.md §5 "Email HTML sanitization", steps 1–3 and the quote
 * marking of step 8. The sandboxed iframe (lib/emailFrame.ts) and its meta CSP are the outer
 * walls; this pipeline removes active content, decides which images may load and counts the
 * blocked ones for the "已隐藏外部图片" banner.
 *
 * Contract (shared with WP-F's lib/quote.ts, which sanitizes `quoted_html` with it):
 *   sanitizeEmailHtml(html, { allowRemote, filesOrigins }) → { html, blockedImages }
 *   - `allowRemote`: false blocks remote images / backgrounds / CSS url()s (counted in
 *     `blockedImages`); true keeps them (remote_images=always, a trusted sender, "显示图片").
 *   - `filesOrigins`: `Me.server.files_origins`; sources on these origins (signed /api/files
 *     URLs, R2) are never blocked. Root-relative `/api/files/…` URLs are made absolute against
 *     the configured API base.
 *   - Additive: `markQuotes: true` tags the trailing quoted block with `data-azm-quote` and sets
 *     `hasQuote` (EmailFrame collapses it behind "…").
 *   - The result is a body fragment for the email iframe's srcdoc or a draft's quote. It keeps
 *     the email's <style> blocks and classes, so it must never be injected into the app
 *     document itself (render it in a sandboxed iframe).
 *
 * Pipeline:
 *  1. DOMParser: <head><style> blocks are moved into the body; the <body> element's
 *     style / bgcolor / background / text become a wrapper <div>.
 *  2. DOMPurify (HTML profile only — no SVG / MathML), FORCE_BODY, FORBID_TAGS (script, iframe,
 *     frame, object, embed, applet, form, input, button, textarea, select, base, meta, link, plus
 *     media and legacy raw-text elements), URI allowlist https/http/mailto/tel/cid (+ data:image
 *     on <img>), event handlers and unknown attributes removed by DOMPurify itself.
 *  3. afterSanitizeAttributes hook:
 *     - <a>/<area>: http(s)/mailto/tel → target=_blank rel="noopener noreferrer nofollow";
 *       "#frag" stays in-frame; anything else loses its href.
 *     - <img src|srcset>, [background], style url(): data:image/*, cid: and files-origin URLs are
 *       kept; remote ones (http, https, protocol-relative) are moved to data-azm-src /
 *       data-azm-srcset / data-azm-background / data-azm-style-src (img gets a 1×1
 *       placeholder, CSS gets `none`) and counted unless `allowRemote`; relative or other URLs
 *       are dropped. "显示图片" re-sanitizes the original with `allowRemote: true`.
 *     - CSS (style attributes and <style> text): escapes decoded first so they cannot hide
 *       tokens; @import removed; remote @font-face dropped (fonts never load under the frame
 *       CSP); expression() / javascript: / -moz-binding / behavior neutralized; position:fixed
 *       and sticky made static; '<' re-escaped inside <style> so the text cannot close the
 *       element when re-parsed.
 *  Email-supplied data-azm-* attributes are removed so mail cannot fake our markers.
 */
import DOMPurify, { type DOMPurify as Purifier } from 'dompurify';
import { resolveApiUrl } from '@/config';

export interface SanitizeOptions {
  allowRemote: boolean;
  filesOrigins: string[];
  /** Additive: mark the trailing quoted block (`data-azm-quote`) and report `hasQuote`. */
  markQuotes?: boolean;
}

export interface SanitizeResult {
  html: string;
  blockedImages: number;
  /** Additive: set when `markQuotes` found a collapsible quoted block. */
  hasQuote?: boolean;
}

/** 1×1 transparent GIF shown in place of a blocked image. */
export const BLOCKED_IMAGE_PLACEHOLDER = 'data:image/gif;base64,R0lGODlhAQABAIAAAAAAAP///yH5BAEAAAAALAAAAAABAAEAAAIBRAA7';

/** Attribute that marks the collapsible quoted block. */
export const QUOTE_ATTR = 'data-azm-quote';

export const FORBID_TAGS = [
  // DESIGN §5 step 2
  'script',
  'iframe',
  'frame',
  'object',
  'embed',
  'applet',
  'form',
  'input',
  'button',
  'textarea',
  'select',
  'base',
  'meta',
  'link',
  // additionally: media (remote fetches outside img), framesets and raw-text legacy elements
  'audio',
  'video',
  'track',
  'frameset',
  'portal',
  'noscript',
  'noembed',
  'noframes',
  'xmp',
  'plaintext',
  'template',
  'dialog',
];

const FORBID_ATTR = [
  'srcdoc',
  'ping',
  'formaction',
  'action',
  'autofocus',
  'tabindex',
  'popover',
  'popovertarget',
  'popovertargetaction',
  'command',
  'commandfor',
  'nonce',
  'integrity',
  'crossorigin',
  'poster',
];

/** DOMPurify's default allowlist minus ftp/callto/sms/xmpp/matrix (the non-scheme alternatives keep plain attribute values like align="center" valid). */
const ALLOWED_URI_REGEXP = /^(?:(?:https?|mailto|tel|cid):|[^a-z]|[a-z+.-]+(?:[^a-z+.\-:]|$))/i;

// ───────────── URL classification ─────────────

type UrlKind = 'data-image' | 'cid' | 'files' | 'remote' | 'fragment' | 'contact' | 'invalid';

interface Classified {
  kind: UrlKind;
  /** Normalized URL to write back (absolute for files / protocol-relative). */
  url: string;
}

interface Ctx {
  allowRemote: boolean;
  origins: Set<string>;
  blocked: number;
}

/** Removes what the URL parser ignores: leading/trailing C0 controls and spaces, inner tab/CR/LF. */
function cleanUrl(raw: string): string {
  // eslint-disable-next-line no-control-regex
  return raw.replace(/^[\u0000- ]+|[\u0000- ]+$/g, '').replace(/[\t\n\r]/g, '');
}

function originOf(url: string): string | null {
  try {
    const u = new URL(url);
    return u.protocol === 'http:' || u.protocol === 'https:' ? u.origin : null;
  } catch {
    return null;
  }
}

function pageHref(): string {
  return typeof window !== 'undefined' && window.location ? window.location.href : 'http://localhost/';
}

/** Normalized origins (lower-case scheme://host[:port]); invalid entries are skipped. */
export function normalizeOrigins(origins: readonly string[]): Set<string> {
  const out = new Set<string>();
  for (const o of origins) {
    const origin = typeof o === 'string' ? originOf(o.trim()) : null;
    if (origin) out.add(origin);
  }
  return out;
}

export function classifyUrl(raw: string, origins: Set<string>): Classified {
  const url = cleanUrl(raw);
  if (!url) return { kind: 'invalid', url };
  const lower = url.toLowerCase();
  if (lower.startsWith('data:')) {
    return /^data:image\/[a-z0-9.+-]+(?:;[^,]*)?,/i.test(url) ? { kind: 'data-image', url } : { kind: 'invalid', url };
  }
  if (lower.startsWith('cid:')) return { kind: 'cid', url };
  if (url.startsWith('#')) return { kind: 'fragment', url };
  if (lower.startsWith('mailto:') || lower.startsWith('tel:')) return { kind: 'contact', url };
  let absolute: string | null = null;
  if (url.startsWith('//')) absolute = `https:${url}`;
  else if (/^https?:/i.test(url)) absolute = url;
  else if (url.startsWith('/api/files/')) {
    try {
      absolute = new URL(resolveApiUrl(url), pageHref()).href;
    } catch {
      return { kind: 'invalid', url };
    }
    return { kind: 'files', url: absolute };
  }
  if (absolute === null) return { kind: 'invalid', url };
  const origin = originOf(absolute);
  if (!origin) return { kind: 'invalid', url };
  return { kind: origins.has(origin) ? 'files' : 'remote', url: absolute };
}

/** Whether an image source may load as-is. */
function imageAllowed(c: Classified, ctx: Ctx): boolean {
  return c.kind === 'data-image' || c.kind === 'cid' || c.kind === 'files' || (c.kind === 'remote' && ctx.allowRemote);
}

// ───────────── CSS ─────────────

const REPLACEMENT_CHAR = '�';

/** Decodes CSS escapes (`\75 rl(` → `url(`) so the scanners below see what the browser sees. */
export function decodeCssEscapes(css: string): string {
  if (!css.includes('\\')) return css;
  return css.replace(/\\(?:([0-9a-fA-F]{1,6})[ \t\n\r\f]?|(\r\n|[\n\r\f])|([\s\S]))/g, (_m, hex?: string, nl?: string, ch?: string) => {
    if (hex !== undefined) {
      const cp = Number.parseInt(hex, 16);
      if (cp === 0 || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return REPLACEMENT_CHAR;
      return String.fromCodePoint(cp);
    }
    if (nl !== undefined) return ''; // escaped newline (string continuation)
    return ch ?? '';
  });
}

const URL_FN_RE = /\burl\(\s*(?:"([^"]*)"|'([^']*)'|([^)"'\s]*))\s*\)/gi;
// NUL delimits url() placeholders (the input never contains NUL: it is mapped to U+FFFD first).
// eslint-disable-next-line no-control-regex
const PLACEHOLDER_RE = /\u0000(\d+)\u0000/g;

interface CssResult {
  css: string;
  /** Number of blocked remote references. */
  blocked: number;
  /** The blocked remote URLs that could be identified (absolute http(s)). */
  blockedUrls: string[];
}

/**
 * Rewrites CSS text. `sheet` = a <style> block (at-rules allowed; '<' escaped), otherwise a
 * style attribute. Blocked remote url()s become `none` and are counted.
 */
export function sanitizeCss(input: string, ctx: Ctx, sheet: boolean): CssResult {
  let css = decodeCssEscapes(input);
  // NUL is our placeholder delimiter below; the CSS parser maps it to U+FFFD anyway.
  // eslint-disable-next-line no-control-regex
  css = css.replace(/\u0000/g, REPLACEMENT_CHAR);
  css = css.replace(/\/\*[\s\S]*?(?:\*\/|$)/g, ' ');
  let blocked = 0;
  const blockedUrls: string[] = [];

  // @import pulls external style sheets (tracking; never allowed under the frame CSP).
  css = css.replace(/@import\s+(?:url\(\s*(?:"[^"]*"|'[^']*'|[^)]*)\s*\)|"[^"]*"|'[^']*')[^;{}]*;?/gi, '');
  css = css.replace(/@import\b/gi, '@azm-removed');

  // Remote web fonts cannot load (font-src data:); drop those @font-face rules entirely.
  css = css.replace(/@font-face\s*\{[^}]*\}/gi, (block) =>
    /url\(\s*(?!["']?\s*data:)/i.test(block) ? '' : block,
  );

  // Neutralize legacy script-in-CSS vectors and fixed overlays.
  css = css
    .replace(/expression\s*\(/gi, 'azm-removed(')
    .replace(/(?:java|vb)script\s*:/gi, 'azm-removed:')
    .replace(/-moz-binding/gi, '-azm-removed')
    .replace(/\bbehaviou?r\s*:/gi, '-azm-removed:')
    .replace(/position\s*:\s*(?:fixed|sticky)/gi, 'position:static');

  // url(): keep allowed sources (normalized), replace the rest with `none`.
  const kept: string[] = [];
  css = css.replace(URL_FN_RE, (_m, dq?: string, sq?: string, bare?: string) => {
    const c = classifyUrl(dq ?? sq ?? bare ?? '', ctx.origins);
    if (c.kind === 'data-image' || c.kind === 'cid' || c.kind === 'files' || (c.kind === 'remote' && ctx.allowRemote)) {
      kept.push(`url("${c.url.replace(/["\\\n\r]/g, (ch) => encodeURIComponent(ch))}")`);
      return `\u0000${kept.length - 1}\u0000`;
    }
    if (c.kind === 'remote') {
      blocked++;
      blockedUrls.push(c.url);
    }
    return 'none';
  });
  // Anything still shaped like a URL-taking function is malformed or string-based
  // (image-set("…"), src("…")): neutralize it unless remote content is allowed.
  if (!ctx.allowRemote) {
    css = css.replace(/\b(url|src|(?:-webkit-)?image-set)\(/gi, (_m, fn: string) => {
      blocked++;
      return `azm-blocked-${fn}(`;
    });
  } else {
    css = css.replace(/\burl\(/gi, 'azm-invalid-url(');
  }
  css = css.replace(PLACEHOLDER_RE, (_m, i: string) => kept[Number(i)] ?? 'none');

  if (sheet) css = css.replace(/</g, '\\3c ');
  return { css, blocked, blockedUrls };
}

// ───────────── attribute handling ─────────────

function moveAside(el: Element, attr: string, value: string): void {
  el.setAttribute(`data-azm-${attr}`, value);
  el.removeAttribute(attr);
}

function sanitizeLink(el: Element, ctx: Ctx): void {
  const href = el.getAttribute('href');
  if (href === null) return;
  const c = classifyUrl(href, ctx.origins);
  switch (c.kind) {
    case 'remote':
    case 'files':
    case 'contact':
      el.setAttribute('href', c.kind === 'contact' ? cleanUrl(href) : c.url);
      el.setAttribute('target', '_blank');
      el.setAttribute('rel', 'noopener noreferrer nofollow');
      break;
    case 'fragment':
      el.setAttribute('target', '_self');
      break;
    default:
      el.removeAttribute('href');
      el.removeAttribute('target');
  }
}

/** srcset candidates are split on ", " (URLs with bare commas, e.g. data:, stay intact). */
function srcsetUrls(srcset: string): string[] {
  return srcset
    .split(/,\s+/)
    .map((c) => c.trim().split(/\s+/)[0] ?? '')
    .filter(Boolean);
}

function sanitizeImage(el: Element, ctx: Ctx): void {
  let blockedHere = false;
  const src = el.getAttribute('src');
  if (src !== null) {
    const c = classifyUrl(src, ctx.origins);
    if (imageAllowed(c, ctx)) {
      if (c.url !== src) el.setAttribute('src', c.url);
    } else if (c.kind === 'remote') {
      moveAside(el, 'src', c.url);
      el.setAttribute('src', BLOCKED_IMAGE_PLACEHOLDER);
      blockedHere = true;
    } else {
      el.removeAttribute('src');
    }
  }
  const srcset = el.getAttribute('srcset');
  if (srcset !== null) {
    const kinds = srcsetUrls(srcset).map((u) => classifyUrl(u, ctx.origins));
    if (kinds.some((c) => c.kind !== 'remote' && !imageAllowed(c, ctx))) {
      el.removeAttribute('srcset'); // relative / script / non-image candidates
    } else if (kinds.some((c) => !imageAllowed(c, ctx))) {
      moveAside(el, 'srcset', srcset);
      blockedHere = true;
    }
  }
  if (blockedHere) {
    el.setAttribute('data-azm-blocked', '');
    ctx.blocked++;
  }
}

function sanitizeBackground(el: Element, ctx: Ctx): void {
  const bg = el.getAttribute('background');
  if (bg === null) return;
  const c = classifyUrl(bg, ctx.origins);
  if (imageAllowed(c, ctx)) {
    if (c.url !== bg) el.setAttribute('background', c.url);
  } else if (c.kind === 'remote') {
    moveAside(el, 'background', c.url);
    ctx.blocked++;
  } else {
    el.removeAttribute('background');
  }
}

function sanitizeStyleAttr(el: Element, ctx: Ctx): void {
  const style = el.getAttribute('style');
  if (style === null) return;
  const { css, blocked, blockedUrls } = sanitizeCss(style, ctx, false);
  if (blocked > 0) {
    // Only the identified URLs are kept aside (never the raw, unsanitized declaration).
    if (blockedUrls.length) el.setAttribute('data-azm-style-src', blockedUrls.join(' '));
    ctx.blocked += blocked;
  }
  if (css !== style) {
    if (css.trim()) el.setAttribute('style', css);
    else el.removeAttribute('style');
  }
}

// ───────────── quoted text ─────────────

const QUOTE_SELECTOR = [
  '.gmail_quote',
  '.gmail_quote_container',
  'blockquote[type="cite"]',
  'blockquote[type="CITE"]',
  '.yahoo_quoted',
  '.protonmail_quote',
  '#divRplyFwdMsg',
].join(',');

const ATTRIBUTION_RE = /(?:wrote|写道|寫道)\s*[:：]?\s*$/i;
const squash = (s: string | null | undefined) => (s ?? '').replace(/\s+/g, '');

/** Length of the visible text under `el` (whitespace and <style> contents excluded). */
function visibleTextLength(el: Element): number {
  let styles = 0;
  for (const s of Array.from(el.querySelectorAll('style'))) styles += squash(s.textContent).length;
  return squash(el.textContent).length - styles;
}

/** Marks the last top-level quoted block (and its attribution line). Returns whether one was marked. */
function markTrailingQuote(root: Element): boolean {
  const candidates = Array.from(root.querySelectorAll(QUOTE_SELECTOR)).filter(
    (el) => !el.parentElement?.closest(QUOTE_SELECTOR),
  );
  const quote = candidates[candidates.length - 1];
  if (!quote) return false;
  // Never hide the whole message (e.g. a forward with no text of its own).
  const outside = visibleTextLength(root) - visibleTextLength(quote);
  if (outside <= 0) return false;
  quote.setAttribute(QUOTE_ATTR, '');
  const prev = quote.previousElementSibling;
  if (
    prev &&
    quote.tagName === 'BLOCKQUOTE' &&
    (prev.classList.contains('moz-cite-prefix') ||
      ((prev.textContent ?? '').trim().length < 300 && ATTRIBUTION_RE.test((prev.textContent ?? '').trim())))
  ) {
    prev.setAttribute(QUOTE_ATTR, '');
  }
  return true;
}

// ───────────── pipeline ─────────────

/** Step 1: hoist <head> styles and keep the <body> element's presentational attributes. */
function prepareInput(html: string): string {
  if (!/<(?:html|head|body|style)\b/i.test(html)) return html;
  const doc = new DOMParser().parseFromString(html, 'text/html');
  const styles = Array.from(doc.head.querySelectorAll('style'))
    .map((s) => s.outerHTML)
    .join('');
  const body = doc.body;
  const wrapper = doc.createElement('div');
  let presentational = false;
  for (const name of ['style', 'bgcolor', 'background', 'class', 'dir', 'lang']) {
    const v = body.getAttribute(name);
    if (v !== null) {
      wrapper.setAttribute(name, v);
      presentational = true;
    }
  }
  const text = body.getAttribute('text');
  if (text !== null && /^#?[0-9a-z]+$/i.test(text.trim())) {
    wrapper.setAttribute('style', `color:${text.trim()};${wrapper.getAttribute('style') ?? ''}`);
    presentational = true;
  }
  if (!presentational) return styles + body.innerHTML;
  while (body.firstChild) wrapper.appendChild(body.firstChild);
  return styles + wrapper.outerHTML;
}

let purifier: Purifier | null = null;
function getPurifier(): Purifier {
  purifier ??= DOMPurify(window);
  return purifier;
}

export function sanitizeEmailHtml(html: string, opts: SanitizeOptions): SanitizeResult {
  if (!html || !html.trim()) return { html: '', blockedImages: 0, ...(opts.markQuotes ? { hasQuote: false } : {}) };
  const ctx: Ctx = { allowRemote: opts.allowRemote, origins: normalizeOrigins(opts.filesOrigins), blocked: 0 };
  const purify = getPurifier();

  purify.removeAllHooks();
  purify.addHook('uponSanitizeAttribute', (_node, data) => {
    // Our own markers are added after sanitizing; the email must not supply them.
    if (data.attrName.startsWith('data-azm-')) data.keepAttr = false;
  });
  purify.addHook('afterSanitizeAttributes', (node) => {
    const tag = node.tagName;
    if (tag === 'A' || tag === 'AREA') sanitizeLink(node, ctx);
    else if (tag === 'IMG' || tag === 'SOURCE') sanitizeImage(node, ctx);
    else if (tag === 'STYLE') {
      const { css, blocked } = sanitizeCss(node.textContent ?? '', ctx, true);
      ctx.blocked += blocked;
      node.textContent = css;
    }
    if (node.hasAttribute('background')) sanitizeBackground(node, ctx);
    if (node.hasAttribute('style')) sanitizeStyleAttr(node, ctx);
  });

  let body: Node;
  try {
    body = purify.sanitize(prepareInput(html), {
      FORCE_BODY: true,
      USE_PROFILES: { html: true },
      FORBID_TAGS,
      FORBID_ATTR,
      ALLOWED_URI_REGEXP,
      ALLOW_DATA_ATTR: true,
      KEEP_CONTENT: true,
      RETURN_DOM: true,
    });
  } finally {
    purify.removeAllHooks();
  }

  const root = body as Element;
  const hasQuote = opts.markQuotes ? markTrailingQuote(root) : undefined;
  const out: SanitizeResult = { html: root.innerHTML, blockedImages: ctx.blocked };
  if (hasQuote !== undefined) out.hasQuote = hasQuote;
  return out;
}
