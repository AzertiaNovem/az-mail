/**
 * Initial field values of a compose window [WP-F] (pure): from a `ComposeInit` plus the loaded
 * parent message (reply / reply-all / forward) or draft, and the current user (`Me`).
 */
import type { Address, Attachment, Draft, DraftMode, Me, Message } from '@/api/types';
import { initialBodyHtml, inlineImageIds } from '@/lib/emailHtml';
import { buildQuotedHtml, safeTimeZone } from '@/lib/quote';
import { defaultFromIdentity, forwardSubject, mineMatcher, replyRecipients, replySubject } from '@/lib/recipients';
import type { ComposeInit } from '@/stores/compose';

export interface ComposeSeed {
  mode: DraftMode;
  parentMessageId: number | null;
  /** Forward: copy the parent's attachments onto the new draft. */
  includeParentAttachments: boolean;
  /** Existing draft (null until the first save). */
  draftId: number | null;
  version: number;
  fromAddressId: number | undefined;
  to: Address[];
  cc: Address[];
  bcc: Address[];
  subject: string;
  html: string;
  quotedHtml: string | null;
  attachments: Attachment[];
  /** Inline attachments referenced by the initial body (see attachmentIdsForSave). */
  editorInlineIds: number[];
  /** Where the caret starts. */
  focus: 'to' | 'body-start' | 'body-end';
}

function newBody(me: Me): string {
  return initialBodyHtml({
    signatureHtml: me.settings.signature_html,
    signatureEnabled: me.settings.signature_enabled,
    filesOrigins: me.server.files_origins,
  });
}

/** Seed of a new message, reply, reply-all or forward. */
export function seedFromInit(init: Exclude<ComposeInit, { kind: 'draft' }>, me: Me, parent: Message | null): ComposeSeed {
  const base = {
    draftId: null,
    version: 0,
    bcc: [] as Address[],
    html: newBody(me),
    attachments: [] as Attachment[],
    editorInlineIds: [] as number[],
  };
  if (init.kind === 'new') {
    return {
      ...base,
      mode: 'new',
      parentMessageId: null,
      includeParentAttachments: false,
      fromAddressId: defaultFromIdentity(me.identities)?.address_id,
      to: init.to ? [...init.to] : [],
      cc: [],
      subject: init.subject ?? '',
      quotedHtml: null,
      focus: init.to && init.to.length ? 'body-start' : 'to',
    };
  }
  const mode: DraftMode = init.kind;
  const quoteOpts = { timeZone: safeTimeZone(me.settings.timezone), filesOrigins: me.server.files_origins };
  const from = defaultFromIdentity(me.identities, parent);
  if (!parent) {
    return {
      ...base,
      mode,
      parentMessageId: init.parentMessageId,
      includeParentAttachments: mode === 'forward',
      fromAddressId: from?.address_id,
      to: [],
      cc: [],
      subject: '',
      quotedHtml: null,
      focus: mode === 'forward' ? 'to' : 'body-start',
    };
  }
  const isMine = mineMatcher(me.identities, [me.email]);
  const recipients = mode === 'forward' ? { to: [], cc: [] } : replyRecipients(parent, mode, isMine);
  return {
    ...base,
    mode,
    parentMessageId: parent.id,
    includeParentAttachments: mode === 'forward',
    fromAddressId: from?.address_id,
    to: recipients.to,
    cc: recipients.cc,
    subject: mode === 'forward' ? forwardSubject(parent.subject) : replySubject(parent.subject),
    quotedHtml: buildQuotedHtml(mode, parent, quoteOpts),
    focus: mode === 'forward' ? 'to' : 'body-start',
  };
}

/** Seed of an existing draft (reopened, restored after reload, or after undo). */
export function seedFromDraft(draft: Draft): ComposeSeed {
  return {
    mode: draft.mode,
    parentMessageId: draft.parent_message_id,
    includeParentAttachments: false,
    draftId: draft.id,
    version: draft.version,
    fromAddressId: draft.from_address_id,
    to: [...draft.to],
    cc: [...draft.cc],
    bcc: [...draft.bcc],
    subject: draft.subject,
    html: draft.html || '<p></p>',
    quotedHtml: draft.quoted_html,
    attachments: [...draft.attachments],
    editorInlineIds: inlineImageIds(draft.html),
    focus: 'body-end',
  };
}

/** The parent message of a reply / forward from a loaded thread. */
export function findParent(messages: readonly Message[] | undefined, parentId: number): Message | null {
  return messages?.find((m) => m.id === parentId) ?? null;
}
