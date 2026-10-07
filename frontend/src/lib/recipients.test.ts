import { describe, expect, it } from 'vitest';
import { makeIdentity, makeMessage } from '@/components/compose/testFixtures';
import {
  addressLabel,
  dedupeAddresses,
  defaultFromIdentity,
  displayAddress,
  formatAddress,
  forwardSubject,
  hasSeparator,
  identityKey,
  isValidEmail,
  mergeAddresses,
  mineMatcher,
  parseAddressList,
  replyRecipients,
  replySubject,
} from './recipients';

const me = makeIdentity({ address_id: 1, email: 'me@az.test' });
const support = makeIdentity({ address_id: 9, email: 'support@az.test', display_name: '客服', kind: 'alias', is_default: false });
const identities = [me, support];
const isMine = mineMatcher(identities, ['me@az.test']);
const A = (email: string, name = '') => ({ name, email });

describe('address validation and formatting', () => {
  it('accepts normal addresses and rejects malformed ones', () => {
    for (const ok of ['a@b.co', 'first.last+tag@sub.example.com', "o'brien@ex.io", 'x_y-z@xn--fiqs8s.xn--fiqz9s'])
      expect(isValidEmail(ok)).toBe(true);
    for (const bad of ['', 'plain', 'a@b', '@b.com', 'a@.com', 'a..b@c.com', '.a@c.com', 'a@b.c', 'a b@c.com', 'a@-b.com', 'a@b.com.', `${'x'.repeat(65)}@b.com`])
      expect(isValidEmail(bad)).toBe(false);
  });

  it('formats display names with RFC 5322 quoting only when needed', () => {
    expect(formatAddress(A('a@b.com', '张三'))).toBe('张三 <a@b.com>');
    expect(formatAddress(A('a@b.com', '张三 (运营)'))).toBe('"张三 (运营)" <a@b.com>');
    expect(formatAddress(A('a@b.com', 'Say "hi"'))).toBe('"Say \\"hi\\"" <a@b.com>');
    expect(formatAddress(A('a@b.com', ''))).toBe('a@b.com');
    expect(formatAddress(A('a@b.com', 'A@B.com'))).toBe('a@b.com');
    expect(displayAddress(A('a@b.com', '张三 (运营)'))).toBe('张三 (运营) <a@b.com>');
    expect(addressLabel(A('a@b.com'))).toBe('a@b.com');
  });

  it('identity keys fold case and drop +tags', () => {
    expect(identityKey(' Alice+News@Example.COM ')).toBe('alice@example.com');
    expect(identityKey('bob@example.com')).toBe('bob@example.com');
  });
});

describe('parseAddressList', () => {
  it('parses a pasted mixed list with ASCII and full-width separators', () => {
    expect(parseAddressList('a@b.com, 张三 <c@d.com>；李四<e@f.cn>，g@h.io')).toEqual([
      A('a@b.com'),
      A('c@d.com', '张三'),
      A('e@f.cn', '李四'),
      A('g@h.io'),
    ]);
  });

  it('keeps commas inside quoted names and angle brackets', () => {
    expect(parseAddressList('"Lee, Ann" <ann@x.com>; Bob <bob@x.com>')).toEqual([A('ann@x.com', 'Lee, Ann'), A('bob@x.com', 'Bob')]);
    expect(parseAddressList('"Say \\"hi\\"" <q@x.com>')).toEqual([A('q@x.com', 'Say "hi"')]);
  });

  it('handles comments, mailto:, space-separated lists and "name address" pairs', () => {
    expect(parseAddressList('a@b.com (Alice)')).toEqual([A('a@b.com', 'Alice')]);
    expect(parseAddressList('mailto:x@y.com')).toEqual([A('x@y.com')]);
    expect(parseAddressList('a@b.com c@d.com')).toEqual([A('a@b.com'), A('c@d.com')]);
    expect(parseAddressList('张三 zs@az.test')).toEqual([A('zs@az.test', '张三')]);
  });

  it('keeps invalid tokens so they can be shown as red chips', () => {
    const out = parseAddressList('not-an-email, ok@b.com');
    expect(out).toEqual([A('not-an-email'), A('ok@b.com')]);
    expect(isValidEmail(out[0]!.email)).toBe(false);
    expect(parseAddressList(' , ;; ')).toEqual([]);
  });

  it('detects separators', () => {
    expect(hasSeparator('a@b.com,')).toBe(true);
    expect(hasSeparator('a@b.com；')).toBe(true);
    expect(hasSeparator('a@b.com')).toBe(false);
  });

  it('dedupes case-insensitively and keeps the first non-empty name', () => {
    expect(dedupeAddresses([A('A@b.com'), A('a@B.com', '甲'), A('c@d.com')])).toEqual([A('A@b.com', '甲'), A('c@d.com')]);
    expect(mergeAddresses([A('a@b.com', 'A')], [A('A@B.COM'), A('n@b.com')])).toEqual([A('a@b.com', 'A'), A('n@b.com')]);
  });
});

describe('replyRecipients', () => {
  it('reply → sender', () => {
    const m = makeMessage({ from: A('x@ext.test', 'X'), to: [A('me@az.test'), A('y@ext.test')] });
    expect(replyRecipients(m, 'reply', isMine)).toEqual({ to: [A('x@ext.test', 'X')], cc: [] });
  });

  it('reply → reply_to[0] when present', () => {
    const m = makeMessage({ from: A('noreply@ext.test'), reply_to: [A('help@ext.test', '客服'), A('other@ext.test')] });
    expect(replyRecipients(m, 'reply', isMine).to).toEqual([A('help@ext.test', '客服')]);
  });

  it('reply-all → sender + To in To, Cc in Cc, minus my identities (incl. aliases and +tags)', () => {
    const m = makeMessage({
      from: A('x@ext.test', 'X'),
      to: [A('ME+work@AZ.test'), A('y@ext.test', 'Y'), A('support@az.test')],
      cc: [A('z@ext.test'), A('me@az.test'), A('Y@ext.test')],
    });
    expect(replyRecipients(m, 'reply_all', isMine)).toEqual({
      to: [A('x@ext.test', 'X'), A('y@ext.test', 'Y')],
      cc: [A('z@ext.test')],
    });
  });

  it('reply-all uses reply_to instead of from and dedupes it against To', () => {
    const m = makeMessage({ from: A('list@ext.test'), reply_to: [A('y@ext.test')], to: [A('y@ext.test', 'Y'), A('me@az.test')] });
    expect(replyRecipients(m, 'reply_all', isMine)).toEqual({ to: [A('y@ext.test', 'Y')], cc: [] });
  });

  it('reply to my own sent message → its original To', () => {
    const m = makeMessage({ direction: 'out', from: A('me@az.test', '张三'), to: [A('bob@ext.test', 'Bob')], cc: [A('cc@ext.test')] });
    expect(replyRecipients(m, 'reply', isMine)).toEqual({ to: [A('bob@ext.test', 'Bob')], cc: [] });
    expect(replyRecipients(m, 'reply_all', isMine)).toEqual({ to: [A('bob@ext.test', 'Bob')], cc: [A('cc@ext.test')] });
  });

  it('a message from me detected by address even when direction is "in" (loopback)', () => {
    const m = makeMessage({ direction: 'in', from: A('support@az.test'), to: [A('cust@ext.test')] });
    expect(replyRecipients(m, 'reply', isMine).to).toEqual([A('cust@ext.test')]);
  });

  it('a note to myself replies to myself', () => {
    const m = makeMessage({ direction: 'out', from: A('me@az.test'), to: [A('me@az.test')] });
    expect(replyRecipients(m, 'reply', isMine).to).toEqual([A('me@az.test')]);
    expect(replyRecipients(m, 'reply_all', isMine)).toEqual({ to: [A('me@az.test')], cc: [] });
  });

  it('reply-all to my own message sent only to myself and others in Cc moves Cc up', () => {
    const m = makeMessage({ direction: 'out', from: A('me@az.test'), to: [A('me@az.test')], cc: [A('c@ext.test')] });
    expect(replyRecipients(m, 'reply_all', isMine)).toEqual({ to: [A('c@ext.test')], cc: [] });
  });

  it('never copies Bcc', () => {
    const m = makeMessage({ direction: 'out', from: A('me@az.test'), to: [A('a@ext.test')], bcc: [A('secret@ext.test')] });
    const r = replyRecipients(m, 'reply_all', isMine);
    expect([...r.to, ...r.cc].map((a) => a.email)).not.toContain('secret@ext.test');
  });
});

describe('defaultFromIdentity', () => {
  it('uses the delivered_to alias when I may send as it', () => {
    expect(defaultFromIdentity(identities, makeMessage({ delivered_to: 'Support@az.test' }))?.address_id).toBe(9);
  });

  it('falls back to my mailbox when delivered_to is an alias I cannot send as', () => {
    expect(defaultFromIdentity(identities, makeMessage({ delivered_to: 'sales@az.test', to: [A('sales@az.test')] }))?.address_id).toBe(1);
  });

  it('keeps the From of my own sent message', () => {
    expect(defaultFromIdentity(identities, makeMessage({ direction: 'out', from: A('support@az.test'), to: [A('c@ext.test')] }))?.address_id).toBe(9);
  });

  it('finds my identity in To / Cc when delivered_to is missing', () => {
    expect(defaultFromIdentity(identities, makeMessage({ to: [A('x@ext.test')], cc: [A('support+t@az.test')] }))?.address_id).toBe(9);
  });

  it('uses the default identity for new messages and handles no identities', () => {
    expect(defaultFromIdentity(identities)?.address_id).toBe(1);
    expect(defaultFromIdentity([support, { ...me, is_default: false }])?.address_id).toBe(1); // user kind before alias
    expect(defaultFromIdentity([])).toBeUndefined();
  });
});

describe('subjects', () => {
  it('adds Re: / Fwd: once, recognising Chinese prefixes', () => {
    expect(replySubject('周报')).toBe('Re: 周报');
    expect(replySubject('Re: 周报')).toBe('Re: 周报');
    expect(replySubject('回复：周报')).toBe('回复：周报');
    expect(replySubject('RE[2]: x')).toBe('RE[2]: x');
    expect(forwardSubject('周报')).toBe('Fwd: 周报');
    expect(forwardSubject('转发: 周报')).toBe('转发: 周报');
    expect(forwardSubject('FW: x')).toBe('FW: x');
    expect(forwardSubject('Re: x')).toBe('Fwd: Re: x');
  });
});
