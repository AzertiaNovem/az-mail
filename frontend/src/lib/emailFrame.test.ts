import { describe, expect, it } from 'vitest';
import { resolveConfig } from '@/config';
import { apiOrigin, buildFrameCsp, buildSrcDoc, EMAIL_FRAME_SANDBOX, frameFilesOrigins, parseMailto } from './emailFrame';
import { sanitizeEmailHtml } from './sanitize';

const ORIGINS = ['https://api.team.test', 'https://acct.r2.cloudflarestorage.com'];

function cspOf(srcdoc: string): string {
  const doc = new DOMParser().parseFromString(srcdoc, 'text/html');
  return doc.querySelector('meta[http-equiv="Content-Security-Policy"]')!.getAttribute('content')!;
}

describe('email frame CSP', () => {
  it('blocks remote images unless allowed; files origins always load', () => {
    expect(buildFrameCsp({ allowRemote: false, filesOrigins: ORIGINS })).toBe(
      "default-src 'none'; img-src data: https://api.team.test https://acct.r2.cloudflarestorage.com; style-src 'unsafe-inline'; font-src data:; form-action 'none'",
    );
    const open = buildFrameCsp({ allowRemote: true, filesOrigins: ORIGINS });
    expect(open).toContain('img-src data: https://api.team.test https://acct.r2.cloudflarestorage.com https: http:;');
    expect(open).not.toContain('script-src');
  });

  it('only lets plain origins into the policy', () => {
    const csp = buildFrameCsp({ allowRemote: false, filesOrigins: ["https://ok.test/path?x", "x; script-src 'unsafe-inline'", 'javascript:alert(1)', 'ftp://f.test'] });
    expect(csp).toBe("default-src 'none'; img-src data: https://ok.test; style-src 'unsafe-inline'; font-src data:; form-action 'none'");
  });
});

describe('buildSrcDoc', () => {
  it('wraps the body with charset, CSP, no-referrer, base target and light base styles', () => {
    const doc = buildSrcDoc('<p>hi</p>', { allowRemote: false, filesOrigins: ORIGINS });
    const parsed = new DOMParser().parseFromString(doc, 'text/html');
    expect(parsed.querySelector('meta[charset]')).not.toBeNull();
    expect(cspOf(doc)).toContain("default-src 'none'");
    expect(parsed.querySelector('meta[name="referrer"]')!.getAttribute('content')).toBe('no-referrer');
    expect(parsed.querySelector('base')!.getAttribute('target')).toBe('_blank');
    expect(parsed.querySelector('base')!.hasAttribute('href')).toBe(false);
    const css = parsed.querySelector('style')!.textContent!;
    expect(css).toContain('color-scheme:light');
    expect(css).toContain('background:#fff');
    expect(css).toContain('img{max-width:100%}');
    expect(css).not.toContain('data-azm-quote');
    expect(parsed.body.innerHTML).toBe('<p>hi</p>');
  });

  it('hides the marked quote only when collapsed', () => {
    const { html } = sanitizeEmailHtml('<p>a</p><blockquote type="cite">b</blockquote>', {
      allowRemote: false,
      filesOrigins: [],
      markQuotes: true,
    });
    const collapsed = buildSrcDoc(html, { allowRemote: false, filesOrigins: [], collapseQuotes: true });
    expect(collapsed).toContain('[data-azm-quote]{display:none!important}');
    expect(buildSrcDoc(html, { allowRemote: false, filesOrigins: [] })).not.toContain('display:none!important');
  });

  it('cannot be broken out of by the CSP attribute', () => {
    const doc = buildSrcDoc('', { allowRemote: false, filesOrigins: ['https://a.test"><script>'] });
    expect(doc).not.toContain('<script>');
  });

  it('uses a sandbox without allow-scripts', () => {
    expect(EMAIL_FRAME_SANDBOX.split(' ')).toEqual(['allow-same-origin', 'allow-popups', 'allow-popups-to-escape-sandbox']);
  });
});

describe('frame origins', () => {
  it('adds the API origin as this page reaches it', () => {
    const sameOrigin = resolveConfig({ apiBase: '' }, { protocol: 'http:', host: 'localhost:3000' });
    expect(apiOrigin(sameOrigin)).toBe(window.location.origin);
    const remote = resolveConfig({ apiBase: 'https://api.team.test/' }, { protocol: 'https:', host: 'mail.team.test' });
    expect(apiOrigin(remote)).toBe('https://api.team.test');
    expect(frameFilesOrigins(ORIGINS, remote)).toEqual(ORIGINS);
    expect(frameFilesOrigins(['https://r2.test'], remote)).toEqual(['https://r2.test', 'https://api.team.test']);
  });
});

describe('parseMailto', () => {
  it('parses recipients, cc / bcc, subject and body (RFC 6068)', () => {
    expect(parseMailto('mailto:a@b.test,c+tag@d.test?subject=%E4%BD%A0%E5%A5%BD%20there&cc=e@f.test&body=line%0A2&bcc=g@h.test')).toEqual({
      to: [
        { name: '', email: 'a@b.test' },
        { name: '', email: 'c+tag@d.test' },
      ],
      cc: [{ name: '', email: 'e@f.test' }],
      bcc: [{ name: '', email: 'g@h.test' }],
      subject: '你好 there',
      body: 'line\n2',
    });
  });

  it('accepts to= in the query, ignores junk and rejects non-mailto URLs', () => {
    expect(parseMailto('MAILTO:?to=x@y.test&subject=%ZZ')).toEqual({ to: [{ name: '', email: 'x@y.test' }], cc: [], bcc: [], subject: '%ZZ', body: '' });
    expect(parseMailto('mailto:not-an-address')!.to).toEqual([]);
    expect(parseMailto('https://x.test')).toBeNull();
    expect(parseMailto('javascript:alert(1)')).toBeNull();
  });
});
