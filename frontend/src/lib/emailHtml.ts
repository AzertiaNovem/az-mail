/**
 * Editor-body HTML helpers [WP-F] (DESIGN.md §1 C10, §5).
 *
 * - Signature: inserted into new bodies ABOVE the quote (the quote lives outside the editor in
 *   `quoted_html`, so "the end of the body" is above it). It is wrapped in
 *   `<div data-azm-signature>` (the Signature editor node) so the signature menu can find,
 *   replace or remove it.
 * - Inline images are `<img src="<signed view_url>" data-att-id="<attachment id>">`; the server
 *   rewrites them to `cid:` on save and strips `data-att-id` when the send is frozen. Images are
 *   never `data:` URIs (pasted images are uploaded first, DESIGN §1 E3).
 * - `attachment_ids` sent with every save: the full list of attachments the draft keeps.
 */
import type { Attachment } from '@/api/types';
import { escapeHtml } from './quote';
import { sanitizeEmailHtml } from './sanitize';

/** Marker attribute of the signature block (parsed by the Signature TipTap node). */
export const SIGNATURE_ATTR = 'data-azm-signature';

function parseBody(html: string): HTMLElement {
  return new DOMParser().parseFromString(`<!doctype html><body>${html}</body>`, 'text/html').body;
}

/** True when the HTML has no visible text and no images (e.g. "<p></p><p><br></p>"). */
export function isBlankHtml(html: string | null | undefined): boolean {
  if (!html) return true;
  const body = parseBody(html);
  if (body.querySelector('img, hr, table')) return false;
  return (body.textContent ?? '').replace(/[\s ​]/g, '') === '';
}

/** Signature HTML made safe for the editor: sanitized, no <style>, no nested signature marker. */
export function sanitizeSignatureHtml(signatureHtml: string, filesOrigins: string[] = []): string {
  const { html } = sanitizeEmailHtml(signatureHtml, { allowRemote: true, filesOrigins });
  const body = parseBody(html);
  for (const el of Array.from(body.querySelectorAll('style, title'))) el.remove();
  for (const el of Array.from(body.querySelectorAll(`[${SIGNATURE_ATTR}]`))) el.removeAttribute(SIGNATURE_ATTR);
  return body.innerHTML.trim();
}

/** `<div data-azm-signature>…</div>`, or '' for a blank signature. */
export function signatureBlockHtml(signatureHtml: string, filesOrigins: string[] = []): string {
  if (isBlankHtml(signatureHtml)) return '';
  return `<div ${SIGNATURE_ATTR}="" class="azm-signature">${sanitizeSignatureHtml(signatureHtml, filesOrigins)}</div>`;
}

export interface InitialBodyOptions {
  signatureHtml: string;
  signatureEnabled: boolean;
  filesOrigins?: string[];
}

/** Body of a brand-new message / reply / forward: two empty lines, then the signature (if enabled). */
export function initialBodyHtml({ signatureHtml, signatureEnabled, filesOrigins = [] }: InitialBodyOptions): string {
  const sig = signatureEnabled ? signatureBlockHtml(signatureHtml, filesOrigins) : '';
  return `<p></p><p></p>${sig}`;
}

export function hasSignatureBlock(html: string): boolean {
  return parseBody(html).querySelector(`[${SIGNATURE_ATTR}]`) !== null;
}

/** Attachment ids of the inline images (`img[data-att-id]`) in `html`, in document order. */
export function inlineImageIds(html: string): number[] {
  const out: number[] = [];
  for (const img of Array.from(parseBody(html).querySelectorAll('img[data-att-id]'))) {
    const id = Number(img.getAttribute('data-att-id'));
    if (Number.isInteger(id) && id > 0 && !out.includes(id)) out.push(id);
  }
  return out;
}

/** The `<img>` inserted for an uploaded inline attachment. */
export function inlineImageHtml(att: Pick<Attachment, 'id' | 'filename' | 'view_url' | 'download_url'>, resolveUrl: (u: string) => string = (u) => u): string {
  const src = resolveUrl(att.view_url ?? att.download_url);
  return `<img src="${escapeHtml(src)}" alt="${escapeHtml(att.filename)}" data-att-id="${att.id}">`;
}

export interface AttachmentIdsInput {
  /** Every attachment the window knows is on (or meant for) the draft. */
  attachments: readonly Pick<Attachment, 'id' | 'inline'>[];
  /** Current editor HTML. */
  editorHtml: string;
  /**
   * Inline attachments that belong to the editor body: uploaded by this window (paste, drop,
   * insert photo) or referenced by the body when the draft was opened. They are dropped once
   * their image is deleted from the body. Other inline attachments (the parent's images the
   * server copied for the quote) are always kept.
   */
  editorOwnedInlineIds: ReadonlySet<number>;
}

/** The `attachment_ids` to save: regular attachments + inline ones still in use. */
export function attachmentIdsForSave({ attachments, editorHtml, editorOwnedInlineIds }: AttachmentIdsInput): number[] {
  const referenced = new Set(inlineImageIds(editorHtml));
  const out: number[] = [];
  for (const a of attachments) {
    if (out.includes(a.id)) continue;
    if (a.inline && editorOwnedInlineIds.has(a.id) && !referenced.has(a.id)) continue;
    out.push(a.id);
  }
  return out;
}
