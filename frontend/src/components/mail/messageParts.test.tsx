import { fireEvent, render, screen } from '@testing-library/react';
import { describe, expect, it, vi } from 'vitest';
import { TooltipProvider } from '@/components/common';
import { authSummary, formatAddress, recipientSummary, replyAllCount } from './addressing';
import { listedAttachments } from './AttachmentList';
import { draftOnlyTarget, replyTarget } from './compose';
import { ApiError } from '@/api/client';
import { deliveryLabel, eventDetail, eventName, rescheduleErrorMessage, statusTone } from './DeliveryStatus';
import { fileIconFor, isImageType } from './fileIcon';
import { labelState } from './LabelMenu';
import { pagerLabel } from './Pager';
import { linkify, PlainTextBody, splitQuoted } from './PlainTextBody';
import { detail, message } from './testFixtures';
import { freshMessages, initialExpanded, inViewScope, olderGroup, partitionMessages } from './ThreadView';

const mine = new Set(['alice@team.test', 'support@team.test']);

describe('plain text bodies', () => {
  it('linkifies URLs and emails, trimming trailing punctuation and CJK text', () => {
    expect(linkify('见 https://wiki.test/a_(b)，或 www.x.test/p. 联系 bob@x.test。')).toEqual([
      { kind: 'text', value: '见 ' },
      { kind: 'url', value: 'https://wiki.test/a_(b)', href: 'https://wiki.test/a_(b)' },
      { kind: 'text', value: '，或 ' },
      { kind: 'url', value: 'www.x.test/p', href: 'https://www.x.test/p' },
      { kind: 'text', value: '. 联系 ' },
      { kind: 'email', value: 'bob@x.test' },
      { kind: 'text', value: '。' },
    ]);
    expect(linkify('(see https://x.test/a)')[1]).toMatchObject({ value: 'https://x.test/a' });
    expect(linkify('no links')).toEqual([{ kind: 'text', value: 'no links' }]);
  });

  it('splits a trailing quote with its attribution line', () => {
    expect(splitQuoted('好的。\n\n在 2026年10月5日，张三 <zs@x> 写道：\n> 原文\n> 第二行\n')).toEqual({
      main: '好的。',
      quoted: '在 2026年10月5日，张三 <zs@x> 写道：\n> 原文\n> 第二行\n',
    });
    expect(splitQuoted('Thanks\r\n-----Original Message-----\r\nFrom: x')).toEqual({ main: 'Thanks', quoted: '-----Original Message-----\nFrom: x' });
    expect(splitQuoted('> only a quote')).toEqual({ main: '> only a quote', quoted: null });
    expect(splitQuoted('> inline\nreply below')).toEqual({ main: '> inline\nreply below', quoted: null });
  });

  it('renders text (never HTML), links and a collapsed quote toggle', () => {
    const onEmail = vi.fn();
    render(
      <TooltipProvider>
        <PlainTextBody text={'<b>not bold</b> https://x.test\n联系 a@b.test\n\nOn Mon, Bob wrote:\n> old'} onEmailClick={onEmail} />
      </TooltipProvider>,
    );
    expect(screen.getByText(/<b>not bold<\/b>/)).toBeInTheDocument();
    expect(document.querySelector('b')).toBeNull();
    const link = screen.getByRole('link', { name: 'https://x.test' });
    expect(link).toHaveAttribute('target', '_blank');
    expect(link).toHaveAttribute('rel', 'noopener noreferrer nofollow');
    fireEvent.click(screen.getByRole('link', { name: 'a@b.test' }));
    expect(onEmail).toHaveBeenCalledWith('a@b.test');
    expect(screen.queryByText('> old')).not.toBeInTheDocument();
    fireEvent.click(screen.getByRole('button', { name: '显示引用的文字' }));
    expect(screen.getByText(/> old/)).toBeInTheDocument();
  });
});

describe('thread view helpers', () => {
  const m = (over: Parameters<typeof message>[1]) => message(1, over);

  it('scopes messages to the view and counts what is hidden', () => {
    const normal = m({ id: 1 });
    const trashed = m({ id: 2, trashed: true });
    const spam = m({ id: 3, is_spam: true });
    expect(inViewScope(trashed, 'trash')).toBe(true);
    expect(inViewScope(trashed, 'inbox')).toBe(false);
    expect(inViewScope(spam, 'spam')).toBe(true);
    expect(partitionMessages([normal, trashed], 'inbox', false)).toEqual({ visible: [normal], hiddenCount: 1, hiddenKind: 'trashed' });
    expect(partitionMessages([normal, spam], null, false)).toMatchObject({ hiddenCount: 1, hiddenKind: 'spam' });
    expect(partitionMessages([normal, trashed], 'trash', false)).toMatchObject({ visible: [trashed], hiddenKind: 'nonTrashed' });
    expect(partitionMessages([normal, trashed], 'inbox', true)).toMatchObject({ visible: [normal, trashed], hiddenCount: 0 });
    // Nothing in scope (e.g. opened from search): show everything.
    expect(partitionMessages([trashed], 'inbox', false)).toMatchObject({ visible: [trashed], hiddenCount: 0 });
  });

  it('expands unread messages and the newest one; collapses the read middle of long threads', () => {
    const msgs = [1, 2, 3, 4, 5, 6].map((id) => m({ id, is_read: id !== 2 }));
    const draft = m({ id: 7, is_draft: true, is_read: false });
    expect([...initialExpanded([...msgs, draft])].sort()).toEqual([2, 6]);
    const expanded = new Set([6]);
    expect(olderGroup(msgs, expanded)).toEqual([1, 3]);
    expect(olderGroup(msgs, new Set([2, 6]))).toBeNull(); // unread #2 breaks the run (length 0)
    expect(olderGroup(msgs.slice(0, 4), expanded)).toBeNull(); // short threads are never folded
  });

  it('F2: a draft that is sent keeps its id but counts as fresh mail (so it gets expanded)', () => {
    const seen = new Set<number>();
    expect(freshMessages([m({ id: 10 }), m({ id: 11, is_draft: true })], seen).map((x) => x.id)).toEqual([10]);
    // Same detail again: nothing new.
    expect(freshMessages([m({ id: 10 }), m({ id: 11, is_draft: true })], seen)).toEqual([]);
    // The reply draft 11 was sent (same row, is_draft=0): fresh now.
    expect(freshMessages([m({ id: 10 }), m({ id: 11 })], seen).map((x) => x.id)).toEqual([11]);
    // Undo send (back to a draft) and send again: fresh again.
    freshMessages([m({ id: 10 }), m({ id: 11, is_draft: true })], seen);
    expect(freshMessages([m({ id: 10 }), m({ id: 11 })], seen).map((x) => x.id)).toEqual([11]);
  });

  it('finds reply and draft-only targets', () => {
    const d = detail(1, [m({ id: 1 }), m({ id: 2 }), m({ id: 3, is_draft: true }), m({ id: 4, trashed: true })]);
    expect(replyTarget(d.messages)!.id).toBe(2);
    expect(draftOnlyTarget(d)).toBeNull();
    const drafts = detail(2, [m({ id: 5, is_draft: true, date: 1 }), m({ id: 6, is_draft: true, date: 9 })]);
    expect(draftOnlyTarget(drafts)!.id).toBe(6);
    expect(draftOnlyTarget(detail(3, []))).toBeNull();
  });
});

describe('addressing and header helpers', () => {
  it('summarizes recipients with 我 and 密送给我', () => {
    const msg = message(1, { to: [{ name: 'Alice', email: 'Alice@team.test' }, { name: 'Bob Smith', email: 'b@x' }], cc: [{ name: '', email: 'c@x' }] });
    expect(recipientSummary(msg, mine)).toBe('我、Bob、c');
    const bccOnly = message(1, { to: [{ name: '张三', email: 'zs@x' }], bcc: [{ name: '', email: 'alice@team.test' }] });
    expect(recipientSummary(bccOnly, mine)).toBe('张三、密送给我');
    const sent = message(1, { direction: 'out', to: [{ name: '', email: 'x@y' }], bcc: [{ name: 'D', email: 'd@y' }] });
    expect(recipientSummary(sent, mine)).toBe('x、D');
  });

  it('counts reply-all recipients without me', () => {
    expect(replyAllCount(message(1, { from: { name: '', email: 'zs@x' }, to: [{ name: '', email: 'alice@team.test' }] }), mine)).toBe(1);
    expect(
      replyAllCount(message(1, { from: { name: '', email: 'zs@x' }, to: [{ name: '', email: 'support@team.test' }], cc: [{ name: '', email: 'ls@x' }] }), mine),
    ).toBe(2);
    expect(replyAllCount(message(1, { reply_to: [{ name: '', email: 'list@x' }], from: { name: '', email: 'zs@x' } }), mine)).toBe(1);
  });

  it('formats addresses and auth results', () => {
    expect(formatAddress({ name: '张三', email: 'zs@x' })).toBe('张三 <zs@x>');
    expect(formatAddress({ name: 'zs@x', email: 'zs@x' })).toBe('zs@x');
    expect(authSummary({ spf: 'pass', dkim: null, dmarc: 'fail' })).toBe('SPF 通过 · DKIM 无 · DMARC 未通过');
    expect(authSummary(null)).toBeNull();
  });

  it('lists attachments except inline images shown in the body', () => {
    const att = (id: number, inline: boolean, cid: string | null) => ({
      id,
      filename: `f${id}`,
      content_type: 'image/png',
      size: 1,
      inline,
      content_id: cid,
      download_url: `/api/files/${id}?d=a`,
      view_url: `/api/files/${id}?d=i`,
    });
    const msg = { html: '<img src="https://api/api/files/1?d=i&sig=x"><img src="cid:c2">', attachments: [att(1, true, 'c1'), att(2, true, 'c2'), att(3, true, 'c3'), att(4, false, null)] };
    expect(listedAttachments(msg).map((a) => a.id)).toEqual([3, 4]);
    expect(listedAttachments({ ...msg, html: null }).map((a) => a.id)).toEqual([1, 2, 3, 4]);
  });

  it('spec F5: maps reschedule failures to a message', () => {
    expect(rescheduleErrorMessage(new ApiError(409, 'already_sent', 'x'))).toBe('邮件已发出，无法更改发送时间');
    expect(rescheduleErrorMessage(new ApiError(409, 'invalid_state', '正在提交定时发送，请稍后重试'))).toBe('正在提交定时发送，请稍后重试');
    expect(rescheduleErrorMessage(new ApiError(422, 'invalid_schedule', '定时发送时间需在 1 分钟后至 30 天内'))).toBe('定时发送时间需在 1 分钟后至 30 天内');
  });

  it('labels delivery statuses and events', () => {
    const ob = { id: 1, status: 'scheduled' as const, status_detail: null, scheduled_at: Date.UTC(2026, 9, 8, 1, 0), scheduled_via: 'resend' as const, undo_until: null, sent_at: null };
    expect(deliveryLabel(ob, Date.UTC(2026, 9, 7, 12, 0), 'Asia/Shanghai')).toBe('已定时 明天 09:00');
    expect(deliveryLabel({ ...ob, status: 'delivered' }, 0, 'Asia/Shanghai')).toBe('已送达');
    expect(statusTone('bounced')).toBe('error');
    expect(statusTone('delivered')).toBe('success');
    expect(eventName('email.delivered')).toBe('已送达');
    expect(eventName('local.canceled')).toBe('已取消');
    expect(eventName('email.something_new')).toBe('email.something_new');
    expect(eventName('other')).toBe('other');
    expect(eventDetail({ type: 'email.bounced', occurred_at: 0, detail: { bounce: { message: '550 no such user' } } })).toBe('550 no such user');
    expect(eventDetail({ type: 'local.failed', occurred_at: 0, detail: { reason: '配额' } })).toBe('配额');
    expect(eventDetail({ type: 'email.sent', occurred_at: 0, detail: {} })).toBeNull();
  });

  it('file icons, label states and pager text', () => {
    expect(fileIconFor('application/pdf', 'a.pdf').icon).toBe('picture_as_pdf');
    expect(fileIconFor('application/octet-stream', 'b.xlsx').icon).toBe('table_chart');
    expect(fileIconFor('application/octet-stream', 'noext').kind).toBe('文件');
    expect(isImageType('image/svg+xml')).toBe(false);
    expect(isImageType('image/jpeg')).toBe(true);
    const targets = [
      { id: 1, label_ids: [1, 2] },
      { id: 2, label_ids: [2] },
    ];
    expect(labelState(targets, 2)).toBe(true);
    expect(labelState(targets, 1)).toBe('indeterminate');
    expect(labelState(targets, 3)).toBe(false);
    expect(labelState([], 1)).toBe(false);
    expect(pagerLabel(0, 50, 1234, true)).toBe('第 1-50 行，共 1,234 行');
    expect(pagerLabel(50, 50, null, true)).toBe('第 51-100 行');
    expect(pagerLabel(100, 7, null, false)).toBe('第 101-107 行，共 107 行');
    expect(pagerLabel(0, 0, 0, false)).toBe('');
  });
});
