/**
 * Indent / outdent [WP-F]. Inside a list the commands nest / lift the list item; elsewhere they
 * step the block's left margin (40 px per level, max 8 levels, `style="margin-left:…"`), which
 * survives in every mail client.
 */
import { Extension } from '@tiptap/core';
import type { EditorState, Transaction } from '@tiptap/pm/state';

export const INDENT_STEP_PX = 40;
export const MAX_INDENT = 8;

declare module '@tiptap/core' {
  interface Commands<ReturnType> {
    indent: {
      indent: () => ReturnType;
      outdent: () => ReturnType;
    };
  }
}

const clamp = (n: number) => Math.max(0, Math.min(MAX_INDENT, n));

export const Indent = Extension.create<{ types: string[] }>({
  name: 'indent',

  addOptions() {
    return { types: ['paragraph', 'heading'] };
  },

  addGlobalAttributes() {
    return [
      {
        types: this.options.types,
        attributes: {
          indent: {
            default: 0,
            parseHTML: (el: HTMLElement) => {
              const px = Number.parseFloat(el.style.marginLeft || '');
              return Number.isFinite(px) && px > 0 ? clamp(Math.round(px / INDENT_STEP_PX)) : 0;
            },
            renderHTML: (attrs: { indent?: number }) =>
              attrs.indent ? { style: `margin-left: ${attrs.indent * INDENT_STEP_PX}px` } : {},
          },
        },
      },
    ];
  },

  addCommands() {
    const step =
      (delta: number) =>
      ({ tr, state, dispatch }: { tr: Transaction; state: EditorState; dispatch?: unknown }) => {
        const { from, to } = state.selection;
        let changed = false;
        state.doc.nodesBetween(from, to, (node, pos) => {
          if (!this.options.types.includes(node.type.name)) return true;
          const current = typeof node.attrs.indent === 'number' ? node.attrs.indent : 0;
          const next = clamp(current + delta);
          if (next !== current) {
            if (dispatch) tr.setNodeMarkup(pos, undefined, { ...node.attrs, indent: next });
            changed = true;
          }
          return false;
        });
        return changed;
      };

    return {
      indent:
        () =>
        (props) => {
          if (props.editor.isActive('listItem')) return props.commands.sinkListItem('listItem');
          return step(1)(props);
        },
      outdent:
        () =>
        (props) => {
          if (props.editor.isActive('listItem')) return props.commands.liftListItem('listItem');
          return step(-1)(props);
        },
    };
  },
});
