import type { ReactNode } from 'react';
import { Icon } from '@/components/common';

export interface EmptyStateProps {
  icon: string;
  title: string;
  hint?: string;
  children?: ReactNode;
}

/** Centered empty-folder message (Gmail style). */
export function EmptyState({ icon, title, hint, children }: EmptyStateProps) {
  return (
    <div className="flex flex-col items-center justify-center px-6 py-16 text-center" role="status">
      <div className="mb-4 flex size-20 items-center justify-center rounded-full bg-surface-container-high text-primary">
        <Icon name={icon} size={40} />
      </div>
      <p className="text-base text-on-surface">{title}</p>
      {hint && <p className="mt-1 max-w-sm text-sm text-on-surface-variant">{hint}</p>}
      {children}
    </div>
  );
}
