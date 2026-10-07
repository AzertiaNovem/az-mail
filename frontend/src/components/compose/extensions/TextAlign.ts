/**
 * Paragraph / heading alignment [WP-F] (`style="text-align:…"`). A small local extension:
 * `@tiptap/extension-text-align` is not a project dependency. Left is the default and is
 * stored as no attribute. Shortcuts follow Gmail: Ctrl/⌘+Shift+L / E / R.
 */
import { Extension } from '@tiptap/core';

export const ALIGNMENTS = ['left', 'center', 'right', 'justify'] as const;
export type Alignment = (typeof ALIGNMENTS)[number];

declare module '@tiptap/core' {
  interface Commands<ReturnType> {
    textAlign: {
      setTextAlign: (alignment: Alignment) => ReturnType;
      unsetTextAlign: () => ReturnType;
    };
  }
}

const isAlignment = (v: string): v is Alignment => (ALIGNMENTS as readonly string[]).includes(v);

export const TextAlign = Extension.create<{ types: string[] }>({
  name: 'textAlign',

  addOptions() {
    return { types: ['paragraph', 'heading'] };
  },

  addGlobalAttributes() {
    return [
      {
        types: this.options.types,
        attributes: {
          textAlign: {
            default: null,
            parseHTML: (el: HTMLElement) => {
              const v = (el.style.textAlign || el.getAttribute('align') || '').toLowerCase();
              return isAlignment(v) && v !== 'left' ? v : null;
            },
            renderHTML: (attrs: { textAlign?: string | null }) =>
              attrs.textAlign ? { style: `text-align: ${attrs.textAlign}` } : {},
          },
        },
      },
    ];
  },

  addCommands() {
    return {
      setTextAlign:
        (alignment) =>
        ({ commands }) =>
          this.options.types
            .map((type) => commands.updateAttributes(type, { textAlign: alignment === 'left' ? null : alignment }))
            .some(Boolean),
      unsetTextAlign:
        () =>
        ({ commands }) =>
          this.options.types.map((type) => commands.resetAttributes(type, 'textAlign')).some(Boolean),
    };
  },

  addKeyboardShortcuts() {
    return {
      'Mod-Shift-l': () => this.editor.commands.setTextAlign('left'),
      'Mod-Shift-e': () => this.editor.commands.setTextAlign('center'),
      'Mod-Shift-r': () => this.editor.commands.setTextAlign('right'),
    };
  },
});
