import * as DM from '@radix-ui/react-dropdown-menu';
import type { ReactElement, ReactNode } from 'react';
import { cx } from './cx';
import { Icon } from './Icon';

export * as DropdownMenuPrimitive from '@radix-ui/react-dropdown-menu';

interface EntryBase {
  /** React key; defaults to the index. */
  key?: string;
}

export interface MenuItemEntry extends EntryBase {
  type?: 'item';
  label: ReactNode;
  icon?: string;
  /** Right-aligned hint, e.g. a keyboard shortcut. */
  shortcut?: string;
  disabled?: boolean;
  /** Error-colored (destructive) item. */
  danger?: boolean;
  /** Call `e.preventDefault()` to keep the menu open. */
  onSelect: (e: Event) => void;
}

export interface MenuCheckboxEntry extends EntryBase {
  type: 'checkbox';
  label: ReactNode;
  checked: boolean | 'indeterminate';
  onCheckedChange: (checked: boolean) => void;
  disabled?: boolean;
  /** Keep the menu open after toggling (label pickers). */
  keepOpen?: boolean;
}

export interface MenuSeparatorEntry extends EntryBase {
  type: 'separator';
}

export interface MenuLabelEntry extends EntryBase {
  type: 'label';
  label: ReactNode;
}

export interface MenuSubmenuEntry extends EntryBase {
  type: 'submenu';
  label: ReactNode;
  icon?: string;
  disabled?: boolean;
  items: MenuEntry[];
}

export type MenuEntry = MenuItemEntry | MenuCheckboxEntry | MenuSeparatorEntry | MenuLabelEntry | MenuSubmenuEntry;

/** Shared classes for composing custom menus from `DropdownMenuPrimitive`. */
export const menuContentClass =
  'z-[1000] min-w-[200px] max-h-[var(--radix-dropdown-menu-content-available-height)] overflow-y-auto ' +
  'rounded-lg bg-surface-container py-2 text-sm text-on-surface shadow-elevation-2 outline-none ' +
  'origin-[var(--radix-dropdown-menu-content-transform-origin)] animate-scale-in';
export const menuItemClass =
  'relative flex h-8 cursor-pointer select-none items-center gap-3 px-4 outline-none ' +
  'data-[highlighted]:bg-on-surface/8 data-[disabled]:pointer-events-none data-[disabled]:opacity-40';
export const menuSeparatorClass = 'my-2 h-px bg-divider';
export const menuLabelClass = 'px-4 py-1.5 text-xs font-medium text-on-surface-variant';

function CheckIndicator({ checked }: { checked: boolean | 'indeterminate' }) {
  return (
    <span
      aria-hidden="true"
      className={cx(
        'inline-flex size-[18px] shrink-0 items-center justify-center rounded-[2px] border-2',
        checked ? 'border-primary bg-primary text-on-primary' : 'border-on-surface-variant',
      )}
    >
      {checked === 'indeterminate' ? <Icon name="remove" size={16} weight={700} /> : checked ? <Icon name="check" size={16} weight={700} /> : null}
    </span>
  );
}

function renderEntries(entries: MenuEntry[]): ReactNode {
  return entries.map((e, i) => {
    const key = e.key ?? String(i);
    switch (e.type) {
      case 'separator':
        return <DM.Separator key={key} className={menuSeparatorClass} />;
      case 'label':
        return (
          <DM.Label key={key} className={menuLabelClass}>
            {e.label}
          </DM.Label>
        );
      case 'checkbox':
        return (
          <DM.CheckboxItem
            key={key}
            className={menuItemClass}
            checked={e.checked}
            disabled={e.disabled}
            onCheckedChange={(c) => e.onCheckedChange(c === true)}
            onSelect={e.keepOpen ? (ev) => ev.preventDefault() : undefined}
          >
            <CheckIndicator checked={e.checked} />
            <span className="min-w-0 flex-1 truncate">{e.label}</span>
          </DM.CheckboxItem>
        );
      case 'submenu':
        return (
          <DM.Sub key={key}>
            <DM.SubTrigger className={cx(menuItemClass, 'data-[state=open]:bg-on-surface/8')} disabled={e.disabled}>
              {e.icon && <Icon name={e.icon} className="text-on-surface-variant" />}
              <span className="min-w-0 flex-1 truncate">{e.label}</span>
              <Icon name="chevron_right" className="text-on-surface-variant" />
            </DM.SubTrigger>
            <DM.Portal>
              <DM.SubContent sideOffset={2} alignOffset={-8} collisionPadding={8} className={menuContentClass}>
                {renderEntries(e.items)}
              </DM.SubContent>
            </DM.Portal>
          </DM.Sub>
        );
      default:
        return (
          <DM.Item
            key={key}
            className={cx(menuItemClass, e.danger && 'text-error')}
            disabled={e.disabled}
            onSelect={e.onSelect}
          >
            {e.icon && <Icon name={e.icon} className={e.danger ? 'text-error' : 'text-on-surface-variant'} />}
            <span className="min-w-0 flex-1 truncate">{e.label}</span>
            {e.shortcut && <span className="ml-4 text-xs text-on-surface-variant">{e.shortcut}</span>}
          </DM.Item>
        );
    }
  });
}

export interface DropdownMenuProps {
  /** Rendered with `asChild`; must accept a ref (e.g. <IconButton/>, <Button/>). */
  trigger: ReactElement;
  items?: MenuEntry[];
  /** Custom content rendered before `items` (e.g. a filter input). */
  children?: ReactNode;
  align?: 'start' | 'center' | 'end';
  side?: 'top' | 'right' | 'bottom' | 'left';
  sideOffset?: number;
  open?: boolean;
  onOpenChange?: (open: boolean) => void;
  modal?: boolean;
  contentClassName?: string;
  /** Accessible name of the menu. */
  'aria-label'?: string;
}

/** Radix dropdown menu with a data-driven item list. */
export function DropdownMenu({
  trigger,
  items = [],
  children,
  align = 'start',
  side = 'bottom',
  sideOffset = 4,
  open,
  onOpenChange,
  modal = false,
  contentClassName,
  'aria-label': ariaLabel,
}: DropdownMenuProps) {
  return (
    <DM.Root open={open} onOpenChange={onOpenChange} modal={modal}>
      <DM.Trigger asChild>{trigger}</DM.Trigger>
      <DM.Portal>
        <DM.Content
          align={align}
          side={side}
          sideOffset={sideOffset}
          collisionPadding={8}
          aria-label={ariaLabel}
          className={cx(menuContentClass, contentClassName)}
        >
          {children}
          {renderEntries(items)}
        </DM.Content>
      </DM.Portal>
    </DM.Root>
  );
}
