import * as P from '@radix-ui/react-popover';
import type { ReactElement, ReactNode } from 'react';
import { cx } from './cx';

export * as PopoverPrimitive from '@radix-ui/react-popover';

/** Surface classes of the popover panel, for custom compositions with `PopoverPrimitive`. */
export const popoverContentClass =
  'z-[1000] max-h-[var(--radix-popover-content-available-height)] overflow-y-auto rounded-lg bg-surface-container ' +
  'text-sm text-on-surface shadow-elevation-2 outline-none ' +
  'origin-[var(--radix-popover-content-transform-origin)] animate-scale-in';

export interface PopoverProps {
  /** Rendered with `asChild`; must accept a ref (e.g. <IconButton/>, <Button/>). */
  trigger: ReactElement;
  children: ReactNode;
  align?: 'start' | 'center' | 'end';
  side?: 'top' | 'right' | 'bottom' | 'left';
  sideOffset?: number;
  open?: boolean;
  onOpenChange?: (open: boolean) => void;
  /** Trap focus and block outside interaction while open (default false). */
  modal?: boolean;
  /** Panel class; replaces the default padding (`p-4`) when given. */
  className?: string;
  /** Accessible name of the panel. */
  'aria-label'?: string;
  /** Called when the panel opens; `e.preventDefault()` keeps focus on the trigger. */
  onOpenAutoFocus?: (e: Event) => void;
}

/**
 * Floating panel anchored to a trigger (Radix Popover): portal, collision handling, Escape and
 * outside click close it. Use for AdvancedSearch, LabelMenu with a filter, SchedulePicker, etc.
 * To anchor to an element other than the trigger, compose `PopoverPrimitive.Anchor` yourself.
 */
export function Popover({
  trigger,
  children,
  align = 'start',
  side = 'bottom',
  sideOffset = 4,
  open,
  onOpenChange,
  modal = false,
  className,
  'aria-label': ariaLabel,
  onOpenAutoFocus,
}: PopoverProps) {
  return (
    <P.Root open={open} onOpenChange={onOpenChange} modal={modal}>
      <P.Trigger asChild>{trigger}</P.Trigger>
      <P.Portal>
        <P.Content
          align={align}
          side={side}
          sideOffset={sideOffset}
          collisionPadding={8}
          aria-label={ariaLabel}
          onOpenAutoFocus={onOpenAutoFocus}
          className={cx(popoverContentClass, className ?? 'p-4')}
        >
          {children}
        </P.Content>
      </P.Portal>
    </P.Root>
  );
}
