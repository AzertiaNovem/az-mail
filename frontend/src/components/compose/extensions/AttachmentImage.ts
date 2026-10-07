/**
 * Image node for mail bodies [WP-F]: TipTap's Image, inline, with a `data-att-id` attribute
 * (DESIGN.md §1 C10). Inline attachments are `<img src="<signed view_url>" data-att-id="<id>">`;
 * the server maps them to `cid:` on save. `data:` URIs are never accepted (allowBase64: false),
 * so pasted base64 images are dropped instead of bloating the message (§1 E3) — pasted image
 * *files* are uploaded by the compose window instead.
 */
import Image from '@tiptap/extension-image';

declare module '@tiptap/core' {
  interface Commands<ReturnType> {
    attachmentImage: {
      /** Inserts an uploaded inline attachment at the selection. */
      insertAttachmentImage: (attrs: { src: string; alt?: string; attId: number }) => ReturnType;
    };
  }
}

export const AttachmentImage = Image.extend({
  addAttributes() {
    return {
      ...this.parent?.(),
      attId: {
        default: null,
        parseHTML: (el: HTMLElement) => {
          const v = el.getAttribute('data-att-id');
          return v && /^\d+$/.test(v) ? v : null;
        },
        renderHTML: (attrs: { attId?: string | number | null }) =>
          attrs.attId === null || attrs.attId === undefined ? {} : { 'data-att-id': String(attrs.attId) },
      },
    };
  },

  addCommands() {
    return {
      ...this.parent?.(),
      insertAttachmentImage:
        ({ src, alt, attId }) =>
        ({ commands }) =>
          commands.insertContent({ type: this.name, attrs: { src, alt: alt ?? null, attId: String(attId) } }),
    };
  },
}).configure({ inline: true, allowBase64: false, HTMLAttributes: {} });
