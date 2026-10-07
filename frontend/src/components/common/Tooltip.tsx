import * as RT from '@radix-ui/react-tooltip';
import { createContext, useContext, type ReactElement, type ReactNode } from 'react';

const HasProvider = createContext(false);

export interface TooltipProviderProps {
  children: ReactNode;
  /** ms before a tooltip opens. */
  delayDuration?: number;
  /** ms within which moving to another trigger opens instantly. */
  skipDelayDuration?: number;
}

/** App-wide provider (mounted in main.tsx). Tooltips also work without it (they self-wrap). */
export function TooltipProvider({ children, delayDuration = 500, skipDelayDuration = 300 }: TooltipProviderProps) {
  return (
    <RT.Provider delayDuration={delayDuration} skipDelayDuration={skipDelayDuration}>
      <HasProvider.Provider value>{children}</HasProvider.Provider>
    </RT.Provider>
  );
}

export interface TooltipProps {
  /** Tooltip text; empty / null renders the child alone. */
  label: ReactNode;
  /** Must accept a ref and DOM props (a DOM element or a component that spreads them). */
  children: ReactElement;
  /** Keyboard shortcut hint, shown after the label. */
  shortcut?: string;
  side?: 'top' | 'right' | 'bottom' | 'left';
  align?: 'start' | 'center' | 'end';
  sideOffset?: number;
  delayDuration?: number;
  disabled?: boolean;
}

/** Gmail-style dark tooltip (Radix). Shown below the trigger by default. */
export function Tooltip({
  label,
  children,
  shortcut,
  side = 'bottom',
  align = 'center',
  sideOffset = 4,
  delayDuration,
  disabled = false,
}: TooltipProps) {
  const hasProvider = useContext(HasProvider);
  if (disabled || label === null || label === undefined || label === '') return children;
  const tip = (
    <RT.Root delayDuration={delayDuration}>
      <RT.Trigger asChild>{children}</RT.Trigger>
      <RT.Portal>
        <RT.Content
          side={side}
          align={align}
          sideOffset={sideOffset}
          collisionPadding={8}
          className="z-[1200] max-w-xs select-none rounded bg-tooltip px-2 py-1 text-xs leading-4 text-white shadow-elevation-1 animate-fade-in"
        >
          {label}
          {shortcut && <span className="ml-1 opacity-70">({shortcut})</span>}
        </RT.Content>
      </RT.Portal>
    </RT.Root>
  );
  return hasProvider ? tip : <RT.Provider>{tip}</RT.Provider>;
}
