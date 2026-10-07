/**
 * Small rich-text editor for the signature [WP-F]: the compose editor's extensions with a
 * compact toolbar (incl. 插入链接). Uncontrolled: `initialHtml` is read once — remount it
 * (`key`) to reset.
 */
import { useEffect, useState } from 'react';
import { cx } from '@/components/common';
import { EditorSurface, useMailEditor } from '@/components/compose/Editor';
import { EditorToolbar } from '@/components/compose/EditorToolbar';
import { LinkDialog } from '@/components/compose/LinkDialog';
import { t } from '@/i18n/zh';

export interface SignatureEditorProps {
  initialHtml: string;
  onChange: (html: string) => void;
  disabled?: boolean;
  invalid?: boolean;
}

export function SignatureEditor({ initialHtml, onChange, disabled, invalid }: SignatureEditorProps) {
  const [linkOpen, setLinkOpen] = useState(false);
  const editor = useMailEditor({
    initialHtml,
    placeholder: '',
    ariaLabel: t('settings.general.signatureEditor'),
    onUpdate: (e) => onChange(e.isEmpty ? '' : e.getHTML()),
  });
  useEffect(() => {
    if (editor && !editor.isDestroyed && editor.isEditable === !!disabled) editor.setEditable(!disabled, false);
  }, [editor, disabled]);
  return (
    <div
      className={cx(
        'overflow-hidden rounded-lg border bg-surface-container transition-colors',
        invalid ? 'border-error' : 'border-outline-variant focus-within:border-primary',
        disabled && 'opacity-60',
      )}
    >
      <div className="azm-signature-editor max-h-64 overflow-y-auto px-3 py-2">
        <EditorSurface editor={editor} />
      </div>
      <EditorToolbar editor={editor} onLink={() => setLinkOpen(true)} className="rounded-none border-t border-divider shadow-none" />
      <LinkDialog editor={editor} open={linkOpen} onOpenChange={setLinkOpen} />
    </div>
  );
}
