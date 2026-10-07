/**
 * Email HTML sanitizer [WP-E]. PLACEHOLDER written by WP0 so WP-F (lib/quote.ts sanitizes
 * `quoted_html` with it) and WP-E can build in parallel; WP-E replaces the body with the full
 * DESIGN.md §5 "Email HTML sanitization" pipeline.
 *
 * Contract (kept by the replacement):
 *   sanitizeEmailHtml(html, { allowRemote, filesOrigins }) → { html, blockedImages }
 *   - `allowRemote`: false blocks remote images / backgrounds / style url()s (counted in
 *     `blockedImages`); true keeps them (remote_images=always or a trusted sender).
 *   - `filesOrigins`: `Me.server.files_origins`; sources on these origins (signed /api/files URLs,
 *     R2) are never blocked.
 *   - The result is a body fragment safe to put in the sandboxed email iframe's srcdoc and to
 *     embed as a quote in a new draft.
 *
 * This placeholder is deliberately conservative: DOMPurify with the spec's FORBID_TAGS, links
 * forced to a new tab, and — when remote content is blocked — remote <img> sources removed and
 * all inline styles / <style> blocks dropped (so no style url() can load).
 */
import DOMPurify from 'dompurify';

export interface SanitizeOptions {
  allowRemote: boolean;
  filesOrigins: string[];
}

export interface SanitizeResult {
  html: string;
  blockedImages: number;
}

const FORBID_TAGS = [
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
];

/** Origin of an absolute or protocol-relative http(s) URL, else null (relative, data:, cid:, …). */
function remoteOrigin(url: string): string | null {
  const u = url.trim();
  if (!/^(https?:)?\/\//i.test(u)) return null;
  try {
    return new URL(u, 'https://placeholder.invalid').origin;
  } catch {
    return null;
  }
}

export function sanitizeEmailHtml(html: string, opts: SanitizeOptions): SanitizeResult {
  // A private instance so the hook never leaks into other DOMPurify users.
  const purify = DOMPurify(window);
  const allowed = new Set(opts.filesOrigins);
  let blockedImages = 0;

  purify.addHook('afterSanitizeAttributes', (node) => {
    if (node.tagName === 'A') {
      node.setAttribute('target', '_blank');
      node.setAttribute('rel', 'noopener noreferrer nofollow');
    }
    if (opts.allowRemote) return;
    if (node.tagName === 'IMG') {
      const origin = remoteOrigin(node.getAttribute('src') ?? '');
      if ((origin !== null && !allowed.has(origin)) || node.hasAttribute('srcset')) {
        node.removeAttribute('src');
        node.removeAttribute('srcset');
        blockedImages++;
      }
    }
    if (node.hasAttribute('background')) {
      node.removeAttribute('background');
      blockedImages++;
    }
  });

  const out = purify.sanitize(html, {
    FORCE_BODY: true,
    FORBID_TAGS: opts.allowRemote ? FORBID_TAGS : [...FORBID_TAGS, 'style'],
    FORBID_ATTR: opts.allowRemote ? [] : ['style'],
  });
  return { html: out, blockedImages };
}
