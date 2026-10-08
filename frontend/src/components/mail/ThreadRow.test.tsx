import { fireEvent, render, screen, within } from '@testing-library/react';
import { MemoryRouter } from 'react-router';
import { describe, expect, it, vi } from 'vitest';
import type { Label, ThreadListItem } from '@/api/types';
import { TooltipProvider } from '@/components/common';
import { participantsView } from './participants';
import { item, LABELS } from './testFixtures';
import { rowStatusChip, ThreadRow, type ThreadRowProps } from './ThreadRow';

const SH = 'Asia/Shanghai';
const NOW = Date.UTC(2026, 9, 7, 12, 0); // 20:00 in Shanghai
const labelsById = new Map<number, Label>(LABELS.map((l) => [l.id, l]));

function renderRow(over: Partial<ThreadListItem> = {}, props: Partial<ThreadRowProps> = {}) {
  const handlers = { onOpen: vi.fn(), onSelect: vi.fn(), onStar: vi.fn(), onAction: vi.fn() };
  const row = item(7, over);
  render(
    <MemoryRouter>
      <TooltipProvider>
        <ThreadRow
          item={row}
          href="/mail/inbox/7"
          folder="inbox"
          currentLabelId={null}
          labelsById={labelsById}
          selected={false}
          focused={false}
          now={NOW}
          tz={SH}
          {...handlers}
          {...props}
        />
      </TooltipProvider>
    </MemoryRouter>,
  );
  return { row, ...handlers, el: document.querySelector('[data-thread-id="7"]') as HTMLElement };
}

describe('participants column', () => {
  const p = (name: string, email: string, extra: { is_me?: boolean; unread?: boolean } = {}) => ({ name, email, is_me: false, unread: false, ...extra });

  it('shows names with 我, the message count and the draft marker', () => {
    const v = participantsView(item(1, { participants: [p('张三', 'zs@x'), p('Alice', 'a@x', { is_me: true })], message_count: 3 }), 'inbox');
    expect(v.parts.map((x) => x.text)).toEqual(['张三', '我']);
    expect(v.count).toBe(3);
    const withDraft = participantsView(item(1, { participants: [p('Bob Smith', 'b@x')], message_count: 1, draft_count: 1 }), 'inbox');
    expect(withDraft.parts.map((x) => [x.text, !!x.draft])).toEqual([
      ['Bob Smith', false],
      ['草稿', true],
    ]);
    expect(withDraft.count).toBe(2);
  });

  it('abbreviates long lists and Latin first names; drafts folder shows only 草稿', () => {
    const many = item(1, {
      participants: [p('Alice Wong', 'a@x'), p('Bob Smith', 'b@x'), p('王五', 'w@x'), p('Carol Jones', 'c@x')],
      message_count: 6,
    });
    expect(participantsView(many, 'inbox').parts.map((x) => x.text)).toEqual(['Alice', '..', '王五', 'Carol']);
    expect(participantsView(item(1, { draft_count: 2, message_count: 0 }), 'drafts')).toMatchObject({
      parts: [{ text: '草稿', draft: true }],
      count: 2,
    });
    expect(participantsView(item(1, { participants: [p('', 'li.si@x')] }), 'inbox').parts[0]!.text).toBe('li.si');
    expect(participantsView(item(1, { participants: [] }), 'inbox').parts[0]!.text).toBe('我');
  });

  it('bolds unread participants of unread threads only', () => {
    const v = participantsView(item(1, { unread: true, participants: [p('A', 'a@x', { unread: true }), p('B', 'b@x')] }), 'inbox');
    expect(v.parts.map((x) => x.bold)).toEqual([true, false]);
  });
});

describe('Sent / Scheduled rows show the recipients (F16)', () => {
  const p = (name: string, email: string, is_me = false) => ({ name, email, is_me });
  const sender = [{ name: 'Alice', email: 'alice@team.test', is_me: true, unread: false }];

  it('"收件人：王五" in Sent and Scheduled; every other folder keeps the senders', () => {
    const row = item(1, { participants: sender, to_preview: [p('王五', 'ww@x.test')] });
    expect(participantsView(row, 'sent')).toMatchObject({ prefix: '收件人：', parts: [{ text: '王五' }], title: '收件人：王五 <ww@x.test>' });
    expect(participantsView(row, 'scheduled').prefix).toBe('收件人：');
    expect(participantsView(row, 'inbox')).toMatchObject({ prefix: null, parts: [{ text: '我' }] });
    expect(participantsView(row, null).prefix).toBeNull();
  });

  it('abbreviates several recipients, marks me and keeps the draft marker and count', () => {
    const row = item(1, {
      participants: sender,
      message_count: 2,
      draft_count: 1,
      to_preview: [p('Bob Smith', 'b@x'), p('王五', 'w@x'), p('Carol Jones', 'c@x'), p('Alice', 'alice@team.test', true)],
    });
    const v = participantsView(row, 'sent');
    expect(v.parts.map((x) => x.text)).toEqual(['Bob', '..', 'Carol', '我', '草稿']);
    expect(v.count).toBe(3);
  });

  it('older servers without to_preview (or no outbound mail) fall back to the senders', () => {
    expect(participantsView(item(1, { participants: sender }), 'sent')).toMatchObject({ prefix: null, parts: [{ text: '我' }] });
    expect(participantsView(item(1, { participants: sender, to_preview: [] }), 'sent').prefix).toBeNull();
  });

  it('renders the prefix in the row', () => {
    renderRow({ participants: sender, to_preview: [p('王五', 'ww@x.test')] }, { folder: 'sent', href: '/mail/sent/7' });
    expect(screen.getByTitle('收件人：王五 <ww@x.test>')).toHaveTextContent('收件人：王五');
  });
});

describe('status chips', () => {
  it('labels scheduled, failed, delayed and pending sends', () => {
    expect(rowStatusChip(item(1, { scheduled_at: Date.UTC(2026, 9, 8, 1, 0), latest_status: 'scheduled' }), 'sent', NOW, SH)).toEqual({
      text: '已定时 明天 09:00',
      tone: 'info',
    });
    expect(rowStatusChip(item(1, { scheduled_at: Date.UTC(2026, 9, 8, 1, 0), latest_status: 'scheduled' }), 'scheduled', NOW, SH)).toBeNull();
    expect(rowStatusChip(item(1, { latest_status: 'bounced' }), 'sent', NOW, SH)).toEqual({ text: '退信', tone: 'error' });
    expect(rowStatusChip(item(1, { latest_status: 'failed' }), 'sent', NOW, SH)!.text).toBe('发送失败');
    expect(rowStatusChip(item(1, { latest_status: 'delivery_delayed' }), 'sent', NOW, SH)!.tone).toBe('warning');
    expect(rowStatusChip(item(1, { latest_status: 'queued' }), 'sent', NOW, SH)!.text).toBe('待发送');
    expect(rowStatusChip(item(1, { latest_status: 'delivered' }), 'sent', NOW, SH)).toBeNull();
    expect(rowStatusChip(item(1), 'inbox', NOW, SH)).toBeNull();
  });
});

describe('ThreadRow', () => {
  it('renders an unread row: bold subject, snippet, label chips and today’s time', () => {
    const { el } = renderRow({
      unread: true,
      subject: '季度周报',
      snippet: '请查收附件',
      label_ids: [1, 2],
      last_at: Date.UTC(2026, 9, 7, 6, 5),
      participants: [{ name: '张三', email: 'zs@x', is_me: false, unread: true }],
    });
    expect(el.className).toContain('row-unread');
    expect(within(el).getByText('季度周报').className).toContain('font-bold');
    expect(within(el).getByText('- 请查收附件', { exact: false })).toBeInTheDocument();
    expect(within(el).getByText('工作')).toBeInTheDocument();
    expect(within(el).getByText('财务')).toBeInTheDocument();
    expect(within(el).getByText('14:05')).toBeInTheDocument();
    const link = within(el).getByRole('link');
    expect(link).toHaveAttribute('href', '/mail/inbox/7');
    expect(link.getAttribute('aria-label')).toMatch(/^未读，张三，季度周报，请查收附件，14:05$/);
  });

  it('renders a read row with the date of an older message and the current label hidden', () => {
    const { el } = renderRow({ last_at: Date.UTC(2026, 2, 3, 4, 0), label_ids: [1] }, { folder: null, currentLabelId: 1 });
    expect(el.className).toContain('row-read');
    expect(within(el).getByText('3月3日')).toBeInTheDocument();
    expect(within(el).queryByText('工作')).not.toBeInTheDocument();
    expect(within(el).getByText('收件箱')).toBeInTheDocument(); // location chip outside the inbox
  });

  it('shows the 草稿 marker in red, attachment chips (max 3) and the subject placeholder', () => {
    const { el } = renderRow({
      subject: ' ',
      draft_count: 1,
      attachments_preview: [
        { id: 1, filename: '报告.pdf', content_type: 'application/pdf' },
        { id: 2, filename: '图.png', content_type: 'image/png' },
        { id: 3, filename: '表.xlsx', content_type: 'application/vnd.ms-excel' },
        { id: 4, filename: '多余.zip', content_type: 'application/zip' },
      ],
      has_attachments: true,
    });
    expect(within(el).getByText('草稿').className).toContain('text-[#d93025]');
    expect(within(el).getByText('(无主题)')).toBeInTheDocument();
    expect(within(el).getByText('报告.pdf')).toBeInTheDocument();
    expect(within(el).getByText('表.xlsx')).toBeInTheDocument();
    expect(within(el).queryByText('多余.zip')).not.toBeInTheDocument();
  });

  it('reports star, select (with shift), hover actions and open', () => {
    const { el, onStar, onSelect, onAction, onOpen, row } = renderRow({ unread: true });
    fireEvent.click(within(el).getByRole('button', { name: '加星标' }));
    expect(onStar).toHaveBeenCalledWith(row);
    fireEvent.click(within(el).getByRole('checkbox', { name: '选择' }), { shiftKey: true });
    expect(onSelect).toHaveBeenCalledWith(row, true, true);
    fireEvent.click(within(el).getByRole('button', { name: '归档' }));
    expect(onAction).toHaveBeenCalledWith(row, 'archive');
    fireEvent.click(within(el).getByRole('button', { name: '标记为已读' }));
    expect(onAction).toHaveBeenCalledWith(row, 'read');
    fireEvent.click(within(el).getByRole('link'));
    expect(onOpen).toHaveBeenCalledWith(row, expect.anything());
  });

  it('offers restore / delete forever in the trash and highlights selection and cursor', () => {
    const { el } = renderRow({ in_inbox: false }, { folder: 'trash', selected: true, focused: true });
    expect(within(el).getByRole('button', { name: '恢复' })).toBeInTheDocument();
    expect(within(el).getByRole('button', { name: '永久删除' })).toBeInTheDocument();
    expect(within(el).queryByRole('button', { name: '归档' })).not.toBeInTheDocument();
    expect(el.className).toContain('row-selected');
    expect(el).toHaveAttribute('aria-current', 'true');
    expect(within(el).getByRole('checkbox')).toHaveAttribute('data-state', 'checked');
  });

  it('uses the two-line card layout on mobile', () => {
    const { el } = renderRow({ subject: '手机布局', snippet: '摘要' }, { mobile: true });
    expect(within(el).getByText('手机布局')).toBeInTheDocument();
    expect(within(el).getByRole('checkbox', { name: '选择' })).toHaveAttribute('aria-checked', 'false');
    expect(screen.queryByRole('button', { name: '归档' })).not.toBeInTheDocument();
  });
});
