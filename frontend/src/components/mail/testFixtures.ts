/** Builders for API objects used by the WP-E tests (not imported by app code). */
import type { Label, Me, Message, ThreadDetail, ThreadListItem, ThreadListResponse } from '@/api/types';

export const ME: Me = {
  id: 1,
  email: 'alice@team.test',
  display_name: 'Alice',
  is_admin: false,
  settings: {
    undo_send_seconds: 5,
    signature_html: '',
    signature_enabled: true,
    timezone: 'Asia/Shanghai',
    page_size: 50,
    remote_images: 'ask',
    trusted_image_senders: [],
    display_name: 'Alice',
  },
  identities: [
    { address_id: 1, email: 'alice@team.test', display_name: 'Alice', kind: 'user', is_default: true },
    { address_id: 2, email: 'support@team.test', display_name: '客服', kind: 'alias', is_default: false },
  ],
  server: { files_origins: ['https://api.team.test'], blob_backend: 'local', version: 'test' },
};

export const LABELS: Label[] = [
  { id: 1, name: '工作', color: '#1a73e8', sort_order: 0 },
  { id: 2, name: '财务', color: '#188038', sort_order: 1 },
];

export function item(id: number, over: Partial<ThreadListItem> = {}): ThreadListItem {
  return {
    id,
    subject: `主题 ${id}`,
    snippet: `摘要 ${id}`,
    participants: [{ name: '张三', email: 'zs@team.test', is_me: false, unread: false }],
    message_count: 1,
    draft_count: 0,
    unread: false,
    starred: false,
    has_attachments: false,
    label_ids: [],
    last_at: Date.UTC(2026, 9, 7, 2, 0),
    in_inbox: true,
    latest_status: null,
    scheduled_at: null,
    attachments_preview: [],
    ...over,
  };
}

export function page(items: ThreadListItem[], over: Partial<ThreadListResponse> = {}): ThreadListResponse {
  return { items, next_cursor: null, total: items.length, ...over };
}

let msgSeq = 100;
export function message(threadId: number, over: Partial<Message> = {}): Message {
  const id = over.id ?? ++msgSeq;
  return {
    id,
    thread_id: threadId,
    direction: 'in',
    is_draft: false,
    from: { name: '张三', email: 'zs@team.test' },
    sent_by: null,
    to: [{ name: 'Alice', email: 'alice@team.test' }],
    cc: [],
    bcc: [],
    reply_to: [],
    delivered_to: null,
    subject: '主题',
    snippet: '摘要',
    date: Date.UTC(2026, 9, 7, 2, 0) + id,
    html: null,
    text: '正文',
    attachments: [],
    is_read: true,
    is_starred: false,
    in_inbox: true,
    is_spam: false,
    trashed: false,
    label_ids: [],
    auth: { spf: 'pass', dkim: 'pass', dmarc: 'pass' },
    warnings: [],
    outbound: null,
    message_id_header: null,
    raw_url: null,
    ...over,
  };
}

export function detail(id: number, messages: Message[], over: Partial<ThreadDetail> = {}): ThreadDetail {
  return { id, subject: `主题 ${id}`, label_ids: [], messages, ...over };
}
