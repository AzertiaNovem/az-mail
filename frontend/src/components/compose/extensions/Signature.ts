/**
 * Signature block node [WP-F]: `<div data-azm-signature>…</div>` holding the user's signature
 * in the editor body (above the quote, which lives outside the editor). The signature menu
 * uses the commands to insert, replace or remove it.
 */
import { mergeAttributes, Node, type Editor } from '@tiptap/core';
import type { Node as PMNode } from '@tiptap/pm/model';
import { SIGNATURE_ATTR, signatureBlockHtml } from '@/lib/emailHtml';

declare module '@tiptap/core' {
  interface Commands<ReturnType> {
    signature: {
      /** Replaces any signature block with `html`, appended at the end of the body. */
      setSignature: (html: string) => ReturnType;
      /** Removes every signature block. */
      removeSignature: () => ReturnType;
    };
  }
}

/** Ranges of the signature blocks in `doc`, last first (so deleting in order keeps positions valid). */
function signatureRanges(doc: PMNode): [number, number][] {
  const ranges: [number, number][] = [];
  doc.descendants((node, pos) => {
    if (node.type.name === 'signature') {
      ranges.push([pos, pos + node.nodeSize]);
      return false;
    }
    return true;
  });
  return ranges.reverse();
}

export function hasSignature(editor: Pick<Editor, 'state'> | null | undefined): boolean {
  return !!editor && signatureRanges(editor.state.doc).length > 0;
}

export const Signature = Node.create({
  name: 'signature',
  group: 'block',
  content: 'block+',
  defining: true,

  parseHTML() {
    return [{ tag: `div[${SIGNATURE_ATTR}]`, priority: 100 }];
  },

  renderHTML({ HTMLAttributes }) {
    return ['div', mergeAttributes(HTMLAttributes, { [SIGNATURE_ATTR]: '', class: 'azm-signature' }), 0];
  },

  addCommands() {
    return {
      removeSignature:
        () =>
        ({ tr, dispatch }) => {
          const ranges = signatureRanges(tr.doc);
          if (ranges.length === 0) return false;
          if (dispatch) for (const [from, to] of ranges) tr.delete(from, to);
          return true;
        },
      setSignature:
        (html) =>
        ({ tr, dispatch, commands }) => {
          const block = signatureBlockHtml(html);
          if (!block) return false;
          if (dispatch) {
            for (const [from, to] of signatureRanges(tr.doc)) tr.delete(from, to);
            // Keep an empty line between the text and the signature, like a new message.
            const last = tr.doc.lastChild;
            const paragraph = tr.doc.type.schema.nodes.paragraph;
            if (paragraph && last && (last.type !== paragraph || last.content.size > 0)) {
              tr.insert(tr.doc.content.size, paragraph.create());
            }
          }
          return commands.insertContentAt(tr.doc.content.size, block, { updateSelection: false });
        },
    };
  },
});
