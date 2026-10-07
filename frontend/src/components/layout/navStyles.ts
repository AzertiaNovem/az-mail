import { cx } from '@/components/common';

/** Sidebar entry classes (Material 3 Gmail): pill on the right, full-width rail item when compact. */
export function navItemClass(active: boolean, compact: boolean): string {
  return cx(
    'flex h-8 items-center text-sm text-on-surface transition-colors duration-100',
    'focus-visible:outline-offset-[-2px]',
    compact ? 'mx-2 w-14 justify-center rounded-full' : 'mr-4 gap-4 rounded-r-full pl-[26px] pr-3',
    active ? 'bg-nav-selected font-bold text-on-nav-selected' : 'hover:bg-nav-hover',
  );
}
