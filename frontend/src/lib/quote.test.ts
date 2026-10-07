import { describe, expect, it } from 'vitest';
import { makeMessage } from '@/components/compose/testFixtures';
import {
  buildForwardQuote,
  buildQuotedHtml,
  buildReplyQuote,
  escapeHtml,
  formatQuoteDate,
  formatZhDateTime,
  quoteAttribution,
  safeTimeZone,
  sanitizeQuotedBody,
  textToHtml,
  zonedParts,
} from './quote';

const opts = { timeZone: 'Asia/Shanghai', filesOrigins: ['https://api.az.test'] };
const WED_2003 = Date.UTC(2026, 9, 7, 12, 3); // 2026-10-07 20:03 in Shanghai

describe('dates', () => {
  it('formats the zh quote date in the display timezone', () => {
    expect(formatQuoteDate(WED_2003, 'Asia/Shanghai')).toBe('2026年10月7日周三 20:03');
    expect(formatQuoteDate(WED_2003, 'America/New_York')).toBe('2026年10月7日周三 08:03');
    expect(formatQuoteDate(WED_2003, 'UTC')).toBe('2026年10月7日周三 12:03');
    // Crossing midnight changes the date and weekday.
    expect(formatQuoteDate(Date.UTC(2026, 9, 7, 16, 30), 'Asia/Shanghai')).toBe('2026年10月8日周四 00:30');
    expect(formatZhDateTime(WED_2003, 'Asia/Shanghai', { omitYear: true })).toBe('10月7日周三 20:03');
  });

  it('falls back to Asia/Shanghai for unknown zones', () => {
    expect(safeTimeZone('Mars/Base')).toBe('Asia/Shanghai');
    expect(safeTimeZone('')).toBe('Asia/Shanghai');
    expect(safeTimeZone('Europe/Paris')).toBe('Europe/Paris');
    expect(zonedParts(WED_2003, 'Mars/Base').hour).toBe(20);
  });
});

describe('escaping', () => {
  it('escapes HTML specials and keeps line breaks of plain text', () => {
    expect(escapeHtml(`<a href="x">'&'</a>`)).toBe('&lt;a href=&quot;x&quot;&gt;&#39;&amp;&#39;&lt;/a&gt;');
    expect(textToHtml('第一行\r\n<b>第二行</b>')).toBe('第一行<br>&lt;b&gt;第二行&lt;/b&gt;');
  });

  it('sanitizes the quoted body: no scripts, handlers, styles or titles', () => {
    const out = sanitizeQuotedBody(
      '<title>T</title><style>body{color:red}</style><p onclick="x()">hi<script>alert(1)</script></p><img src="https://api.az.test/api/files/3?d=i" data-att-id="3">',
      opts.filesOrigins,
    );
    expect(out).not.toMatch(/script|onclick|<style|<title|color:red/);
    expect(out).toContain('<p>hi</p>');
    expect(out).toContain('data-att-id="3"');
  });
});

describe('reply quote', () => {
  it('builds the zh attribution with escaped sender and blockquote', () => {
    const parent = makeMessage({ from: { name: '张三 <x>', email: 'a@b.com' }, date: WED_2003, html: '<p>原文 & 内容</p>' });
    expect(quoteAttribution(parent, 'Asia/Shanghai')).toBe('在 2026年10月7日周三 20:03，张三 <x> <a@b.com> 写道：');
    const html = buildReplyQuote(parent, opts);
    expect(html).toContain('在 2026年10月7日周三 20:03，张三 &lt;x&gt; &lt;a@b.com&gt; 写道：');
    expect(html).toMatch(/<blockquote class="gmail_quote"[^>]*><p>原文 &amp; 内容<\/p><\/blockquote>/);
    expect(html.startsWith('<div class="gmail_quote azm_quote">')).toBe(true);
  });

  it('uses the bare address when there is no name and escapes plain-text bodies', () => {
    const parent = makeMessage({ from: { name: '', email: 'a@b.com' }, html: null, text: '1 < 2\n第二行', date: WED_2003 });
    const html = buildReplyQuote(parent, opts);
    expect(html).toContain('，a@b.com 写道：');
    expect(html).toContain('1 &lt; 2<br>第二行');
  });
});

describe('forward quote', () => {
  it('lists 发件人 / 日期 / 主题 / 收件人 / 抄送 and escapes them', () => {
    const parent = makeMessage({
      from: { name: '李四', email: 'li@ext.test' },
      to: [{ name: '张三', email: 'me@az.test' }, { name: '', email: 'x@y.com' }],
      cc: [{ name: 'C&C', email: 'c@y.com' }],
      subject: '<周报>',
      date: WED_2003,
    });
    const html = buildForwardQuote(parent, opts);
    expect(html).toContain('---------- 转发的邮件 ---------');
    expect(html).toContain('发件人：<strong class="gmail_sendername" dir="auto">李四</strong> <span dir="auto">&lt;li@ext.test&gt;</span>');
    expect(html).toContain('日期：2026年10月7日周三 20:03');
    expect(html).toContain('主题：&lt;周报&gt;');
    expect(html).toContain('收件人：张三 &lt;me@az.test&gt;, x@y.com');
    expect(html).toContain('抄送：C&amp;C &lt;c@y.com&gt;');
    expect(html).toContain('<p>原文</p>');
  });

  it('omits 抄送 when there is no Cc', () => {
    expect(buildForwardQuote(makeMessage({ cc: [] }), opts)).not.toContain('抄送：');
  });

  it('picks the builder by mode', () => {
    const parent = makeMessage();
    expect(buildQuotedHtml('new', parent, opts)).toBeNull();
    expect(buildQuotedHtml('reply', null, opts)).toBeNull();
    expect(buildQuotedHtml('reply_all', parent, opts)).toContain('写道：');
    expect(buildQuotedHtml('forward', parent, opts)).toContain('转发的邮件');
  });
});
