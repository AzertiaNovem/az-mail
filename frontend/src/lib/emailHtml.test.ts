import { describe, expect, it } from 'vitest';
import {
  attachmentIdsForSave,
  hasSignatureBlock,
  initialBodyHtml,
  inlineImageHtml,
  inlineImageIds,
  isBlankHtml,
  sanitizeSignatureHtml,
  signatureBlockHtml,
} from './emailHtml';

describe('emailHtml', () => {
  it('detects blank bodies', () => {
    expect(isBlankHtml('')).toBe(true);
    expect(isBlankHtml(null)).toBe(true);
    expect(isBlankHtml('<p></p><p><br></p><p>&nbsp;</p>')).toBe(true);
    expect(isBlankHtml('<p>x</p>')).toBe(false);
    expect(isBlankHtml('<p><img src="a.png"></p>')).toBe(false);
  });

  it('builds new bodies with the signature above the (external) quote', () => {
    expect(initialBodyHtml({ signatureHtml: '<p>张三</p>', signatureEnabled: true })).toBe(
      '<p></p><p></p><div data-azm-signature="" class="azm-signature"><p>张三</p></div>',
    );
    expect(initialBodyHtml({ signatureHtml: '<p>张三</p>', signatureEnabled: false })).toBe('<p></p><p></p>');
    expect(initialBodyHtml({ signatureHtml: '<p><br></p>', signatureEnabled: true })).toBe('<p></p><p></p>');
    expect(hasSignatureBlock(initialBodyHtml({ signatureHtml: 'x', signatureEnabled: true }))).toBe(true);
  });

  it('sanitizes signatures (no scripts / styles / nested markers)', () => {
    const s = sanitizeSignatureHtml('<style>p{}</style><p onclick="x">a<script>1</script></p><div data-azm-signature>b</div>');
    expect(s).toBe('<p>a</p><div>b</div>');
    expect(signatureBlockHtml('')).toBe('');
  });

  it('lists inline image ids in order without duplicates', () => {
    const html = '<p><img src="a" data-att-id="7"><img src="b" data-att-id="3"><img src="c" data-att-id="7"><img src="d"><img data-att-id="x" src="e"></p>';
    expect(inlineImageIds(html)).toEqual([7, 3]);
  });

  it('renders inline images with data-att-id and escaped attributes', () => {
    const html = inlineImageHtml({ id: 5, filename: 'a"b.png', view_url: '/api/files/5?d=i&u=1', download_url: '/x' }, (u) => `https://api${u}`);
    expect(html).toBe('<img src="https://api/api/files/5?d=i&amp;u=1" alt="a&quot;b.png" data-att-id="5">');
  });

  it('keeps regular attachments and inline ones still in the body or not owned by the editor', () => {
    const attachments = [
      { id: 1, inline: false },
      { id: 2, inline: true }, // pasted, still in the body
      { id: 3, inline: true }, // pasted, deleted from the body
      { id: 4, inline: true }, // parent image copied for the quote (not editor-owned)
      { id: 1, inline: false }, // duplicate
    ];
    const ids = attachmentIdsForSave({
      attachments,
      editorHtml: '<p><img src="x" data-att-id="2"></p>',
      editorOwnedInlineIds: new Set([2, 3]),
    });
    expect(ids).toEqual([1, 2, 4]);
  });
});
