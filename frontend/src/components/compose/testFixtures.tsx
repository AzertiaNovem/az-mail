/**
 * Test fixtures for the WP-F tests (not bundled: only *.test.* files import it).
 */
import { QueryClient, QueryClientProvider } from '@tanstack/react-query';
import { render } from '@testing-library/react';
import type { ReactElement, ReactNode } from 'react';
import { createMemoryRouter, RouterProvider } from 'react-router';
import { queryKeys } from '@/api/queryKeys';
import type { Attachment, Draft, Identity, Me, Message } from '@/api/types';
import { TooltipProvider } from '@/components/common';

export const ME_ID = 1;

export function makeIdentity(p: Partial<Identity> = {}): Identity {
  return { address_id: 1, email: 'me@az.test', display_name: '张三', kind: 'user', is_default: true, ...p };
}

export function makeMe(p: Partial<Me> = {}): Me {
  return {
    id: ME_ID,
    email: 'me@az.test',
    display_name: '张三',
    is_admin: false,
    settings: {
      undo_send_seconds: 5,
      signature_html: '',
      signature_enabled: true,
      timezone: 'Asia/Shanghai',
      page_size: 50,
      remote_images: 'ask',
      trusted_image_senders: [],
      display_name: '张三',
    },
    identities: [makeIdentity()],
    server: { files_origins: ['https://api.az.test'], blob_backend: 'local', version: '1.0.0' },
    ...p,
  };
}

export function makeMessage(p: Partial<Message> = {}): Message {
  return {
    id: 100,
    thread_id: 10,
    direction: 'in',
    is_draft: false,
    from: { name: '李四', email: 'lisi@ext.test' },
    sent_by: null,
    to: [{ name: '张三', email: 'me@az.test' }],
    cc: [],
    bcc: [],
    reply_to: [],
    delivered_to: null,
    subject: '周报',
    snippet: '',
    date: Date.UTC(2026, 9, 7, 12, 3), // 2026-10-07 20:03 Asia/Shanghai (Wednesday)
    html: '<p>原文</p>',
    text: null,
    attachments: [],
    is_read: true,
    is_starred: false,
    in_inbox: true,
    is_spam: false,
    trashed: false,
    label_ids: [],
    auth: null,
    warnings: [],
    outbound: null,
    message_id_header: '<m1@ext.test>',
    raw_url: null,
    ...p,
  };
}

export function makeDraft(p: Partial<Draft> = {}): Draft {
  return {
    id: 500,
    thread_id: 10,
    version: 1,
    mode: 'new',
    parent_message_id: null,
    from_address_id: 1,
    to: [],
    cc: [],
    bcc: [],
    subject: '',
    html: '<p></p>',
    quoted_html: null,
    attachments: [],
    updated_at: 0,
    ...p,
  };
}

export function makeAttachment(p: Partial<Attachment> = {}): Attachment {
  return {
    id: 70,
    filename: 'a.pdf',
    content_type: 'application/pdf',
    size: 1000,
    inline: false,
    content_id: null,
    download_url: '/api/files/70?d=a',
    view_url: '/api/files/70?d=i',
    ...p,
  };
}

export function makeQueryClient(me: Me | null = makeMe()): QueryClient {
  const qc = new QueryClient({ defaultOptions: { queries: { retry: false }, mutations: { retry: false } } });
  if (me) qc.setQueryData(queryKeys.me(), me);
  return qc;
}

/** Renders `ui` inside QueryClient + data router (+ tooltips); extra routes can be added. */
export function renderWithProviders(
  ui: ReactElement,
  { qc = makeQueryClient(), path = '/', routes = [] as { path: string; element: ReactNode }[] } = {},
) {
  const router = createMemoryRouter(
    [
      { path: '*', element: ui },
      ...routes,
    ],
    { initialEntries: [path] },
  );
  const utils = render(
    <QueryClientProvider client={qc}>
      <TooltipProvider>
        <RouterProvider router={router} />
      </TooltipProvider>
    </QueryClientProvider>,
  );
  return { ...utils, qc, router };
}

/** A file of `size` bytes without allocating them. */
export function fakeFile(name: string, size: number, type = 'application/octet-stream'): File {
  const f = new File(['x'], name, { type });
  Object.defineProperty(f, 'size', { value: size });
  return f;
}

/**
 * jsdom has no layout: ProseMirror's scroll-into-view needs Range/Element client rects.
 * Install once per test file that focuses an editor.
 */
export function installEditorDomPolyfills(): void {
  const emptyRects = () => {
    const list: DOMRect[] = [];
    return Object.assign(list, { item: () => null }) as unknown as DOMRectList;
  };
  const zeroRect = () => new DOMRect(0, 0, 0, 0);
  if (typeof Range !== 'undefined') {
    if (!Range.prototype.getClientRects) Range.prototype.getClientRects = emptyRects;
    if (!Range.prototype.getBoundingClientRect) Range.prototype.getBoundingClientRect = zeroRect;
  }
  if (typeof document !== 'undefined' && !document.elementFromPoint) {
    document.elementFromPoint = () => null;
  }
}
