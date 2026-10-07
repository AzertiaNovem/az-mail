/**
 * Gmail-style formatting toolbar [WP-F]: 撤销/重做 · 字体大小 · 粗体/斜体/下划线 · 文字颜色 ·
 * 对齐 · 编号/项目符号列表 · 缩进 · 引用 · 删除线 · 清除格式 (· 插入链接 when `onLink` is given).
 * Buttons keep the editor selection (mousedown is prevented) and reflect the active marks.
 */
import { useEditorState } from '@tiptap/react';
import { useState, type ComponentProps, type ReactNode } from 'react';
import { cx, DropdownMenu, Icon, IconButton, Popover, Tooltip } from '@/components/common';
import { t } from '@/i18n/zh';
import type { Editor } from './Editor';
import type { Alignment } from './extensions/TextAlign';

/** Gmail's text color palette (8 × 8). */
export const TEXT_COLORS = [
  ['#000000', '#444444', '#666666', '#999999', '#cccccc', '#eeeeee', '#f3f3f3', '#ffffff'],
  ['#ff0000', '#ff9900', '#ffff00', '#00ff00', '#00ffff', '#0000ff', '#9900ff', '#ff00ff'],
  ['#f4cccc', '#fce5cd', '#fff2cc', '#d9ead3', '#d0e0e3', '#cfe2f3', '#d9d2e9', '#ead1dc'],
  ['#ea9999', '#f9cb9c', '#ffe599', '#b6d7a8', '#a2c4c9', '#9fc5e8', '#b4a7d6', '#d5a6bd'],
  ['#e06666', '#f6b26b', '#ffd966', '#93c47d', '#76a5af', '#6fa8dc', '#8e7cc3', '#c27ba0'],
  ['#cc0000', '#e69138', '#f1c232', '#6aa84f', '#45818e', '#3d85c6', '#674ea7', '#a64d79'],
  ['#990000', '#b45f06', '#bf9000', '#38761d', '#134f5c', '#0b5394', '#351c75', '#741b47'],
  ['#660000', '#783f04', '#7f6000', '#274e13', '#0c343d', '#073763', '#20124d', '#4c1130'],
] as const;

/** 小 / 正常 / 大 / 巨大 → inline font sizes (Normal = no mark). */
export const FONT_SIZES = [
  { key: 'small', size: '12px' },
  { key: 'normal', size: null },
  { key: 'large', size: '18px' },
  { key: 'huge', size: '32px' },
] as const;

const SIZE_LABELS = {
  small: 'compose.toolbar.sizeSmall',
  normal: 'compose.toolbar.sizeNormal',
  large: 'compose.toolbar.sizeLarge',
  huge: 'compose.toolbar.sizeHuge',
} as const;

const ALIGN_ITEMS: { value: Alignment; icon: string; label: 'compose.toolbar.alignLeft' | 'compose.toolbar.alignCenter' | 'compose.toolbar.alignRight' }[] = [
  { value: 'left', icon: 'format_align_left', label: 'compose.toolbar.alignLeft' },
  { value: 'center', icon: 'format_align_center', label: 'compose.toolbar.alignCenter' },
  { value: 'right', icon: 'format_align_right', label: 'compose.toolbar.alignRight' },
];

const isMac = typeof navigator !== 'undefined' && /Mac|iPhone|iPad/.test(navigator.platform || navigator.userAgent);
/** Shortcut hint with the platform's modifier, e.g. "⌘B" / "Ctrl+B". */
export const shortcut = (key: string, shift = false): string =>
  isMac ? `${shift ? '⇧' : ''}⌘${key}` : `Ctrl+${shift ? 'Shift+' : ''}${key}`;

interface ToolbarState {
  canUndo: boolean;
  canRedo: boolean;
  bold: boolean;
  italic: boolean;
  underline: boolean;
  strike: boolean;
  bulletList: boolean;
  orderedList: boolean;
  blockquote: boolean;
  link: boolean;
  align: Alignment;
  color: string | null;
  fontSize: string | null;
}

const EMPTY: ToolbarState = {
  canUndo: false,
  canRedo: false,
  bold: false,
  italic: false,
  underline: false,
  strike: false,
  bulletList: false,
  orderedList: false,
  blockquote: false,
  link: false,
  align: 'left',
  color: null,
  fontSize: null,
};

function readState(editor: Editor | null): ToolbarState {
  if (!editor || editor.isDestroyed) return EMPTY;
  const align = (['center', 'right', 'justify'] as const).find((a) => editor.isActive({ textAlign: a })) ?? 'left';
  const style = editor.getAttributes('textStyle') as { color?: string | null; fontSize?: string | null };
  return {
    canUndo: editor.can().undo(),
    canRedo: editor.can().redo(),
    bold: editor.isActive('bold'),
    italic: editor.isActive('italic'),
    underline: editor.isActive('underline'),
    strike: editor.isActive('strike'),
    bulletList: editor.isActive('bulletList'),
    orderedList: editor.isActive('orderedList'),
    blockquote: editor.isActive('blockquote'),
    link: editor.isActive('link'),
    align,
    color: style.color ?? null,
    fontSize: style.fontSize ?? null,
  };
}

function Divider() {
  return <span aria-hidden="true" className="mx-1 h-5 w-px shrink-0 bg-divider" />;
}

interface ToggleProps {
  icon: string;
  label: string;
  active?: boolean;
  disabled?: boolean;
  keys?: string;
  onClick: () => void;
}

function Tool({ icon, label, active, disabled, keys, onClick }: ToggleProps) {
  return (
    <IconButton
      icon={icon}
      label={label}
      size="sm"
      shortcut={keys}
      tooltipSide="top"
      active={active}
      disabled={disabled}
      onClick={onClick}
      className="azm-tool"
    />
  );
}

/** Text-color trigger: glyph + a bar in the current color. Spreads Radix trigger props onto the button. */
function ColorButton({ swatch, className, ...rest }: Omit<ComponentProps<'button'>, 'color'> & { swatch: string | null }) {
  return (
    <Tooltip label={t('compose.toolbar.textColor')} side="top">
      <button
        {...rest}
        type="button"
        aria-label={t('compose.toolbar.textColor')}
        className={cx(
          'azm-tool relative inline-flex size-8 shrink-0 items-center justify-center rounded-full text-on-surface-variant',
          'transition-colors hover:bg-on-surface/8 focus-visible:bg-on-surface/12 data-[state=open]:bg-on-surface/8',
          className,
        )}
      >
        <Icon name="format_color_text" size={18} />
        <span
          aria-hidden="true"
          className="absolute bottom-[5px] left-1/2 h-[3px] w-4 -translate-x-1/2 rounded-sm border border-black/10"
          style={{ backgroundColor: swatch ?? '#1f1f1f' }}
        />
      </button>
    </Tooltip>
  );
}

function ColorPicker({ editor, current }: { editor: Editor; current: string | null }) {
  const [open, setOpen] = useState(false);
  const apply = (color: string | null) => {
    const chain = editor.chain().focus();
    if (color) chain.setColor(color).run();
    else chain.unsetColor().run();
    setOpen(false);
  };
  const currentKey = current?.toLowerCase() ?? null;
  return (
    <Popover
      open={open}
      onOpenChange={setOpen}
      side="top"
      align="center"
      aria-label={t('compose.toolbar.textColor')}
      className="p-3"
      onOpenAutoFocus={(e) => e.preventDefault()}
      trigger={<ColorButton swatch={current} />}
    >
      <div className="flex flex-col gap-2">
        <button
          type="button"
          className="self-start rounded px-2 py-1 text-xs text-on-surface-variant hover:bg-on-surface/8"
          onClick={() => apply(null)}
        >
          {t('compose.toolbar.defaultColor')}
        </button>
        <div className="grid grid-cols-8 gap-1">
          {TEXT_COLORS.flat().map((c) => (
            <button
              key={c}
              type="button"
              aria-label={t('compose.toolbar.colorSwatch', { color: c })}
              aria-pressed={currentKey === c}
              title={c}
              onClick={() => apply(c)}
              className={cx(
                'size-[18px] rounded-sm border border-black/10 transition-transform hover:scale-110',
                currentKey === c && 'ring-2 ring-primary ring-offset-1',
              )}
              style={{ backgroundColor: c }}
            />
          ))}
        </div>
      </div>
    </Popover>
  );
}

export interface EditorToolbarProps {
  editor: Editor | null;
  /** Shows an 插入链接 button (the compose window has it in its action row instead). */
  onLink?: () => void;
  className?: string;
  /** Extra content at the end (e.g. a close button). */
  trailing?: ReactNode;
}

export function EditorToolbar({ editor, onLink, className, trailing }: EditorToolbarProps) {
  const s = useEditorState({ editor, selector: ({ editor: e }) => readState(e) }) ?? EMPTY;
  if (!editor) return null;
  const run = (fn: (c: ReturnType<Editor['chain']>) => ReturnType<Editor['chain']>) => () => fn(editor.chain().focus()).run();
  const sizeKey = FONT_SIZES.find((f) => f.size === s.fontSize)?.key ?? 'normal';
  const alignIcon = ALIGN_ITEMS.find((a) => a.value === s.align)?.icon ?? 'format_align_left';

  return (
    <div
      role="toolbar"
      tabIndex={-1}
      aria-label={t('compose.toolbar.label')}
      className={cx('azm-toolbar', className)}
      // Keep the editor's selection when clicking toolbar buttons.
      onMouseDown={(e) => {
        if ((e.target as HTMLElement).closest('button')) e.preventDefault();
      }}
    >
      <Tool icon="undo" label={t('compose.toolbar.undo')} keys={shortcut('Z')} disabled={!s.canUndo} onClick={run((c) => c.undo())} />
      <Tool icon="redo" label={t('compose.toolbar.redo')} keys={shortcut('Y')} disabled={!s.canRedo} onClick={run((c) => c.redo())} />
      <Divider />
      <DropdownMenu
        side="top"
        aria-label={t('compose.toolbar.fontSize')}
        trigger={
          <IconButton icon="format_size" label={t('compose.toolbar.fontSize')} size="sm" tooltipSide="top" className="azm-tool" />
        }
        items={FONT_SIZES.map((f) => ({
          key: f.key,
          label: (
            <span className="flex items-center gap-3">
              <Icon name="check" className={cx('text-on-surface-variant', f.key !== sizeKey && 'invisible')} />
              <span style={f.size ? { fontSize: f.size === '32px' ? '20px' : f.size } : undefined}>{t(SIZE_LABELS[f.key])}</span>
            </span>
          ),
          onSelect: () => {
            const chain = editor.chain().focus();
            if (f.size) chain.setFontSize(f.size).run();
            else chain.unsetFontSize().run();
          },
        }))}
      />
      <Divider />
      <Tool icon="format_bold" label={t('compose.toolbar.bold')} keys={shortcut('B')} active={s.bold} onClick={run((c) => c.toggleBold())} />
      <Tool icon="format_italic" label={t('compose.toolbar.italic')} keys={shortcut('I')} active={s.italic} onClick={run((c) => c.toggleItalic())} />
      <Tool
        icon="format_underlined"
        label={t('compose.toolbar.underline')}
        keys={shortcut('U')}
        active={s.underline}
        onClick={run((c) => c.toggleUnderline())}
      />
      <ColorPicker editor={editor} current={s.color} />
      <Divider />
      <DropdownMenu
        side="top"
        aria-label={t('compose.toolbar.align')}
        trigger={<IconButton icon={alignIcon} label={t('compose.toolbar.align')} size="sm" tooltipSide="top" className="azm-tool" />}
        items={ALIGN_ITEMS.map((a) => ({
          key: a.value,
          label: t(a.label),
          icon: a.icon,
          onSelect: () => editor.chain().focus().setTextAlign(a.value).run(),
        }))}
      />
      <Tool
        icon="format_list_numbered"
        label={t('compose.toolbar.numberedList')}
        keys={shortcut('7', true)}
        active={s.orderedList}
        onClick={run((c) => c.toggleOrderedList())}
      />
      <Tool
        icon="format_list_bulleted"
        label={t('compose.toolbar.bulletList')}
        keys={shortcut('8', true)}
        active={s.bulletList}
        onClick={run((c) => c.toggleBulletList())}
      />
      <Tool icon="format_indent_decrease" label={t('compose.toolbar.indentLess')} onClick={run((c) => c.outdent())} />
      <Tool icon="format_indent_increase" label={t('compose.toolbar.indentMore')} onClick={run((c) => c.indent())} />
      <Tool icon="format_quote" label={t('compose.toolbar.quote')} active={s.blockquote} onClick={run((c) => c.toggleBlockquote())} />
      <Tool icon="strikethrough_s" label={t('compose.toolbar.strike')} keys={shortcut('S', true)} active={s.strike} onClick={run((c) => c.toggleStrike())} />
      <Tool icon="format_clear" label={t('compose.toolbar.clear')} onClick={run((c) => c.unsetAllMarks().unsetTextAlign())} />
      {onLink && (
        <>
          <Divider />
          <Tool icon="link" label={t('compose.toolbar.link')} keys={shortcut('K')} active={s.link} onClick={onLink} />
        </>
      )}
      {trailing}
    </div>
  );
}
