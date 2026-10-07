import { describe, expect, it } from 'vitest';
import {
  BLOCKED_IMAGE_PLACEHOLDER,
  classifyUrl,
  decodeCssEscapes,
  normalizeOrigins,
  QUOTE_ATTR,
  sanitizeEmailHtml,
} from './sanitize';

const FILES = 'https://api.example.com';
const blocked = (html: string) => sanitizeEmailHtml(html, { allowRemote: false, filesOrigins: [FILES] });
const allowed = (html: string) => sanitizeEmailHtml(html, { allowRemote: true, filesOrigins: [FILES] });

/** Parses sanitized output the way the iframe will (fresh parse of the serialized string). */
function reparse(html: string): HTMLElement {
  return new DOMParser().parseFromString(`<!DOCTYPE html><body>${html}`, 'text/html').body;
}

/** Every element and attribute in the reparsed output: no handlers, no script URLs, no active tags. */
function assertInert(html: string) {
  const body = reparse(html);
  for (const el of Array.from(body.querySelectorAll('*'))) {
    expect(['SCRIPT', 'IFRAME', 'OBJECT', 'EMBED', 'FORM', 'BASE', 'META', 'LINK', 'SVG', 'MATH']).not.toContain(
      el.tagName.toUpperCase(),
    );
    for (const a of Array.from(el.attributes)) {
      expect(a.name.startsWith('on'), `${el.tagName}[${a.name}]`).toBe(false);
      expect(/^\s*(?:javascript|vbscript|data:text)/i.test(a.value), `${el.tagName}[${a.name}]=${a.value}`).toBe(false);
      expect(a.name).not.toBe('srcdoc');
    }
  }
}

describe('sanitizeEmailHtml — XSS vectors', () => {
  const vectors: [string, string][] = [
    ['script element', '<p>hi</p><script>alert(1)</script>'],
    ['mixed-case script', '<ScRiPt>alert(1)</sCrIpT><p>x</p>'],
    ['img onerror', '<img src=x onerror=alert(1)>'],
    ['event handlers', '<div onclick="alert(1)" onmouseover="alert(2)">x</div>'],
    ['javascript: href', '<a href="javascript:alert(1)">x</a>'],
    ['obfuscated javascript: href', '<a href="  JaVaScRiPt:alert(1)">x</a><a href="jav&#x09;ascript:alert(1)">y</a>'],
    ['vbscript: href', '<a href="vbscript:msgbox(1)">x</a>'],
    ['data:text/html href', '<a href="data:text/html;base64,PHNjcmlwdD5hbGVydCgxKTwvc2NyaXB0Pg==">x</a>'],
    ['data:text/html img', '<img src="data:text/html,<script>alert(1)</script>">'],
    ['svg onload', '<svg onload="alert(1)"><circle r="5"/></svg>'],
    ['svg image href', '<svg><image href="https://evil.test/x.png"/><use href="https://evil.test/s.svg#a"/></svg>'],
    ['svg script', '<svg><script>alert(1)</script></svg>'],
    ['math mXSS', '<math><mtext><table><mglyph><style><img src=x onerror=alert(1)></style></mglyph></table></mtext></math>'],
    ['iframe srcdoc', '<iframe srcdoc="<script>alert(1)</script>"></iframe>'],
    ['iframe src', '<iframe src="https://evil.test/"></iframe><frame src="https://evil.test/">'],
    ['object / embed / applet', '<object data="data:text/html,x"></object><embed src="https://evil.test/x.swf"><applet code="x"></applet>'],
    ['form + controls', '<form action="https://evil.test/"><input name="pw"><button formaction="https://evil.test">Go</button><textarea>t</textarea><select><option>o</option></select></form>'],
    ['meta refresh', '<meta http-equiv="refresh" content="0;url=https://evil.test/">'],
    ['base href', '<base href="https://evil.test/"><a href="x">rel</a>'],
    ['link stylesheet', '<link rel="stylesheet" href="https://evil.test/x.css">'],
    ['style expression()', '<div style="width: expression(alert(1))">x</div>'],
    ['style javascript url', '<div style="background:url(javascript:alert(1))">x</div>'],
    ['escaped style url', '<div style="\\62 ackground: \\75 rl(javascript:alert(1))">x</div>'],
    ['-moz-binding', '<div style="-moz-binding:url(https://evil.test/x.xml#x)">x</div>'],
    ['noscript mXSS', '<noscript><p title="</noscript><img src=x onerror=alert(1)>"></noscript>'],
    ['template', '<template><script>alert(1)</script></template><p>ok</p>'],
    ['video poster / audio', '<video poster="https://evil.test/p.png" src="https://evil.test/v.mp4"></video><audio src="https://evil.test/a.mp3"></audio>'],
    ['srcset javascript', '<img srcset="javascript:alert(1) 1x">'],
    ['area javascript', '<map name="m"><area href="javascript:alert(1)" shape="rect" coords="0,0,1,1"></map>'],
    ['comment breakout', '<!--<img src=x onerror=alert(1)>--><p>x</p>'],
  ];

  it.each(vectors)('%s', (_name, input) => {
    const out = blocked(input).html;
    assertInert(out);
    expect(out).not.toMatch(/<script|onerror=|onload=|onclick=|javascript:|vbscript:|expression\(|-moz-binding/i);
  });

  it('runs on at least 25 vectors', () => {
    expect(vectors.length).toBeGreaterThanOrEqual(25);
  });

  it('keeps the text content of removed form controls', () => {
    const out = blocked('<form><button>提交</button></form>').html;
    expect(out).toContain('提交');
    expect(out).not.toMatch(/<button|<form/i);
  });

  it('a <style> text cannot close its element after CSS escapes are decoded', () => {
    const out = blocked('<style>a{content:"\\3c/style\\3e\\3cimg src=x onerror=alert(1)\\3e"}</style><p>x</p>').html;
    const body = reparse(out);
    expect(body.querySelector('img')).toBeNull();
    expect(body.querySelector('style')).not.toBeNull();
    assertInert(out);
  });

  it('strips @import and neutralizes image-set() / escaped url() when blocking', () => {
    const r = blocked(
      '<style>@import url(https://evil.test/a.css); @import "https://evil.test/b.css"; p{color:red}' +
        '.x{background:image-set("https://evil.test/i.png" 1x)}</style>' +
        '<div style="\\62 ackground:\\75 rl(https://evil.test/t.png)">y</div>',
    );
    expect(r.html).not.toMatch(/@import/i);
    expect(r.html).toContain('p{color:red}');
    expect(r.html).not.toMatch(/(?<!blocked-)image-set\(/);
    expect(r.html).not.toContain('evil.test/t.png)');
    expect(r.blockedImages).toBeGreaterThanOrEqual(2);
  });

  it('drops remote @font-face (fonts never load in the frame) without counting it', () => {
    const r = blocked('<style>@font-face{font-family:x;src:url(https://evil.test/f.woff2)} p{font-family:x}</style><p>t</p>');
    expect(r.html).not.toContain('@font-face');
    expect(r.blockedImages).toBe(0);
    const keep = blocked('<style>@font-face{font-family:y;src:url("data:font/woff2;base64,AAAA")}</style><p>t</p>');
    expect(keep.html).toContain('@font-face');
  });

  it('makes fixed / sticky overlays static', () => {
    const out = blocked('<div style="position: fixed; top:0; left:0">phish</div><style>.a{position:sticky}</style>').html;
    expect(out).not.toMatch(/position:\s*(fixed|sticky)/i);
    expect(out).toContain('position:static');
  });

  it('removes email-supplied data-azm-* markers', () => {
    const out = blocked(`<div ${QUOTE_ATTR}="" data-azm-src="x" data-keep="1">t</div>`).html;
    expect(out).not.toContain('data-azm-');
    expect(out).toContain('data-keep="1"');
  });
});

describe('sanitizeEmailHtml — links', () => {
  it('opens http(s) links in a new tab without opener / referrer', () => {
    const a = reparse(blocked('<a href="https://ok.test/p" target="_top">x</a>').html).querySelector('a')!;
    expect(a.getAttribute('href')).toBe('https://ok.test/p');
    expect(a.getAttribute('target')).toBe('_blank');
    expect(a.getAttribute('rel')).toBe('noopener noreferrer nofollow');
  });

  it('keeps mailto / tel, keeps #fragments in-frame and drops relative or cid hrefs', () => {
    const body = reparse(
      blocked('<a href="mailto:a@b.test">m</a><a href="tel:+861234">t</a><a href="#sec">f</a><a href="/x">r</a><a href="cid:x">c</a>').html,
    );
    const [m, t, f, r, c] = Array.from(body.querySelectorAll('a'));
    expect(m!.getAttribute('href')).toBe('mailto:a@b.test');
    expect(t!.getAttribute('href')).toBe('tel:+861234');
    expect(f!.getAttribute('href')).toBe('#sec');
    expect(f!.getAttribute('target')).toBe('_self');
    expect(r!.hasAttribute('href')).toBe(false);
    expect(c!.hasAttribute('href')).toBe(false);
  });
});

describe('sanitizeEmailHtml — remote images', () => {
  it('blocks remote <img> into data-azm-src with a placeholder and counts it', () => {
    const r = blocked('<img src="https://tracker.test/p.gif" width="1" height="1" alt="t">');
    const img = reparse(r.html).querySelector('img')!;
    expect(img.getAttribute('src')).toBe(BLOCKED_IMAGE_PLACEHOLDER);
    expect(img.getAttribute('data-azm-src')).toBe('https://tracker.test/p.gif');
    expect(img.getAttribute('width')).toBe('1');
    expect(r.blockedImages).toBe(1);
  });

  it('treats protocol-relative and http URLs as remote', () => {
    const r = blocked('<img src="//cdn.test/a.png"><img src="http://cdn.test/b.png">');
    expect(r.blockedImages).toBe(2);
    expect(r.html).toContain('data-azm-src="https://cdn.test/a.png"');
  });

  it('blocks srcset, background attributes and style url()s', () => {
    const r = blocked(
      '<img src="cid:a" srcset="https://cdn.test/a.png 1x, https://cdn.test/b.png 2x">' +
        '<table background="https://cdn.test/bg.png"><tr><td style="background-image:url(\'https://cdn.test/c.png\')">x</td></tr></table>' +
        '<style>.h{background:url("https://cdn.test/d.png") no-repeat}</style>',
    );
    const body = reparse(r.html);
    expect(body.querySelector('img')!.hasAttribute('srcset')).toBe(false);
    expect(body.querySelector('img')!.getAttribute('data-azm-srcset')).toContain('https://cdn.test/a.png');
    expect(body.querySelector('table')!.hasAttribute('background')).toBe(false);
    expect(body.querySelector('table')!.getAttribute('data-azm-background')).toBe('https://cdn.test/bg.png');
    expect(body.querySelector('td')!.getAttribute('style')).toBe('background-image:none');
    expect(body.querySelector('td')!.getAttribute('data-azm-style-src')).toBe('https://cdn.test/c.png');
    expect(body.querySelector('style')!.textContent).toContain('background:none no-repeat');
    expect(r.blockedImages).toBe(4);
  });

  it('keeps everything when remote images are allowed', () => {
    const r = allowed('<img src="https://cdn.test/a.png"><div style="background:url(https://cdn.test/b.png)">x</div>');
    expect(r.blockedImages).toBe(0);
    const body = reparse(r.html);
    expect(body.querySelector('img')!.getAttribute('src')).toBe('https://cdn.test/a.png');
    expect(body.querySelector('div')!.getAttribute('style')).toBe('background:url("https://cdn.test/b.png")');
    expect(r.html).not.toContain('data-azm-');
  });

  it('never blocks cid:, data:image and files-origin (signed) URLs', () => {
    const signed = `${FILES}/api/files/12?d=i&u=1&exp=99&sig=abc`;
    const r = blocked(
      `<img src="cid:logo@x"><img src="data:image/png;base64,iVBORw0KGgo="><img src="${signed}">` +
        `<div style="background:url(${signed})">x</div>`,
    );
    expect(r.blockedImages).toBe(0);
    const imgs = Array.from(reparse(r.html).querySelectorAll('img'));
    expect(imgs.map((i) => i.getAttribute('src'))).toEqual(['cid:logo@x', 'data:image/png;base64,iVBORw0KGgo=', signed]);
    expect(reparse(r.html).querySelector('div')!.getAttribute('style')).toBe(`background:url("${signed}")`);
  });

  it('resolves root-relative /api/files URLs and drops other relative sources', () => {
    const r = blocked('<img src="/api/files/7?d=i&amp;sig=x"><img src="images/a.png">');
    const imgs = Array.from(reparse(r.html).querySelectorAll('img'));
    expect(imgs[0]!.getAttribute('src')).toBe(`${window.location.origin}/api/files/7?d=i&sig=x`);
    expect(imgs[1]!.hasAttribute('src')).toBe(false);
    expect(r.blockedImages).toBe(0);
  });
});

describe('sanitizeEmailHtml — document structure', () => {
  it('hoists <head> styles and keeps <body> presentation on a wrapper', () => {
    const r = blocked(
      '<html><head><title>T</title><style>.c{color:blue}</style></head><body bgcolor="#eeeeee" style="margin:0" onload="alert(1)"><p class="c">x</p></body></html>',
    );
    const body = reparse(r.html);
    expect(body.querySelector('style')!.textContent).toContain('.c{color:blue}');
    const wrapper = body.querySelector('div')!;
    expect(wrapper.getAttribute('bgcolor')).toBe('#eeeeee');
    expect(wrapper.getAttribute('style')).toBe('margin:0');
    expect(wrapper.querySelector('p.c')).not.toBeNull();
    expect(r.html).not.toContain('onload');
  });

  it('returns an empty result for empty input', () => {
    expect(blocked('')).toEqual({ html: '', blockedImages: 0 });
    expect(sanitizeEmailHtml('  ', { allowRemote: false, filesOrigins: [], markQuotes: true })).toEqual({
      html: '',
      blockedImages: 0,
      hasQuote: false,
    });
  });

  it('marks the trailing quote (and its attribution line) when asked', () => {
    const html =
      '<p>好的，收到。</p><div>On Mon, Alice wrote:</div><blockquote type="cite"><p>原文</p><blockquote type="cite">更早</blockquote></blockquote>';
    const r = sanitizeEmailHtml(html, { allowRemote: false, filesOrigins: [], markQuotes: true });
    expect(r.hasQuote).toBe(true);
    const body = reparse(r.html);
    const marked = Array.from(body.querySelectorAll(`[${QUOTE_ATTR}]`));
    expect(marked.map((m) => m.tagName)).toEqual(['DIV', 'BLOCKQUOTE']);
    expect(body.querySelector('p')!.hasAttribute(QUOTE_ATTR)).toBe(false);
  });

  it('marks Gmail quotes but never hides a message that is only a quote', () => {
    const r = sanitizeEmailHtml('<div>hi</div><div class="gmail_quote">quoted</div>', {
      allowRemote: false,
      filesOrigins: [],
      markQuotes: true,
    });
    expect(r.hasQuote).toBe(true);
    const only = sanitizeEmailHtml('<style>p{}</style><div class="gmail_quote">only</div>', {
      allowRemote: false,
      filesOrigins: [],
      markQuotes: true,
    });
    expect(only.hasQuote).toBe(false);
    expect(only.html).not.toContain(QUOTE_ATTR);
  });
});

describe('sanitize helpers', () => {
  it('classifies URLs', () => {
    const o = normalizeOrigins([FILES, 'not a url', 'HTTPS://R2.Example.com/']);
    expect([...o]).toEqual([FILES, 'https://r2.example.com']);
    expect(classifyUrl('https://r2.example.com/x', o).kind).toBe('files');
    expect(classifyUrl(' https://other.test/x ', o)).toEqual({ kind: 'remote', url: 'https://other.test/x' });
    expect(classifyUrl('//other.test/x', o).url).toBe('https://other.test/x');
    expect(classifyUrl('data:image/svg+xml;base64,AA', o).kind).toBe('data-image');
    expect(classifyUrl('data:text/html,x', o).kind).toBe('invalid');
    expect(classifyUrl('java\nscript:alert(1)', o).kind).toBe('invalid');
    expect(classifyUrl('ftp://x.test/a', o).kind).toBe('invalid');
  });

  it('decodes CSS escapes', () => {
    expect(decodeCssEscapes('\\75 rl(\\"x)')).toBe('url("x)');
    expect(decodeCssEscapes('\\0 a')).toBe('\uFFFDa');
    expect(decodeCssEscapes('plain')).toBe('plain');
  });
});
