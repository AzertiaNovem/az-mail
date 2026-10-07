import { Editor } from '@tiptap/core';
import { describe, expect, it } from 'vitest';
import { createMailExtensions } from './Editor';
import { hasSignature } from './extensions/Signature';
import { normalizeLinkUrl } from './LinkDialog';
import { installEditorDomPolyfills } from './testFixtures';

installEditorDomPolyfills();

const make = (content: string) => new Editor({ extensions: createMailExtensions('正文'), content });

describe('mail editor extensions', () => {
  it('keeps data-att-id on images and drops data: URIs', () => {
    const ed = make('<p>a<img src="https://api/x.png" data-att-id="3"><img src="data:image/png;base64,AAAA"></p>');
    expect(ed.getHTML()).toBe('<p>a<img src="https://api/x.png" data-att-id="3"></p>');
    ed.commands.insertAttachmentImage({ src: 'https://api/y.png', alt: 'y', attId: 9 });
    expect(ed.getHTML()).toContain('<img src="https://api/y.png" alt="y" data-att-id="9">');
    ed.destroy();
  });

  it('inserts, replaces and removes the signature block at the end', () => {
    const ed = make('<p>Hello</p>');
    expect(hasSignature(ed)).toBe(false);
    ed.commands.setSignature('<p>— 张三</p>');
    expect(ed.getHTML()).toBe('<p>Hello</p><p></p><div data-azm-signature="" class="azm-signature"><p>— 张三</p></div>');
    expect(hasSignature(ed)).toBe(true);
    ed.commands.setSignature('<p>新签名</p>');
    expect(ed.getHTML().match(/data-azm-signature/g)).toHaveLength(1);
    expect(ed.getHTML()).toContain('新签名');
    ed.commands.removeSignature();
    expect(hasSignature(ed)).toBe(false);
    expect(ed.commands.removeSignature()).toBe(false);
    expect(ed.commands.setSignature('<p><br></p>')).toBe(false); // blank signature
    ed.destroy();
  });

  it('parses a stored signature block', () => {
    const ed = make('<p></p><div data-azm-signature=""><p><strong>张三</strong></p></div>');
    expect(hasSignature(ed)).toBe(true);
    ed.destroy();
  });

  it('aligns and indents paragraphs with inline styles', () => {
    const ed = make('<p>x</p>');
    ed.commands.setTextSelection(1);
    ed.commands.setTextAlign('center');
    ed.commands.indent();
    ed.commands.indent();
    expect(ed.getHTML()).toMatch(/^<p style="text-align: center; margin-left: 80px;?">x<\/p>$/);
    ed.commands.outdent();
    ed.commands.setTextAlign('left');
    expect(ed.getHTML()).toMatch(/^<p style="margin-left: 40px;?">x<\/p>$/);
    // Parsing pasted styles.
    const pasted = make('<p style="text-align:right;margin-left:120px">y</p>');
    expect(pasted.getJSON().content?.[0]?.attrs).toMatchObject({ textAlign: 'right', indent: 3 });
    ed.destroy();
    pasted.destroy();
  });

  it('applies colors and font sizes as inline text styles', () => {
    const ed = make('<p>abc</p>');
    ed.chain().setTextSelection({ from: 1, to: 4 }).setColor('#cc0000').setFontSize('18px').run();
    expect(ed.getHTML()).toMatch(/<span style="color: (#cc0000|rgb\(204, 0, 0\)); font-size: 18px;?">abc<\/span>/);
    ed.destroy();
  });
});

describe('normalizeLinkUrl', () => {
  it('accepts web and mail links, rejects scripts', () => {
    expect(normalizeLinkUrl('https://example.com/a?b=1')).toBe('https://example.com/a?b=1');
    expect(normalizeLinkUrl('example.com/x')).toBe('https://example.com/x');
    expect(normalizeLinkUrl('  www.qq.com ')).toBe('https://www.qq.com');
    expect(normalizeLinkUrl('a@b.com')).toBe('mailto:a@b.com');
    expect(normalizeLinkUrl('mailto:a@b.com?subject=hi')).toBe('mailto:a@b.com?subject=hi');
    expect(normalizeLinkUrl('javascript:alert(1)')).toBeNull();
    expect(normalizeLinkUrl('data:text/html,x')).toBeNull();
    expect(normalizeLinkUrl('not a url')).toBeNull();
    expect(normalizeLinkUrl('')).toBeNull();
    expect(normalizeLinkUrl('mailto:nope')).toBeNull();
  });
});
