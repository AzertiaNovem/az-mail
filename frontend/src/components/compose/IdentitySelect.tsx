/**
 * "发件人" row [WP-F]: picks one of the identities the user may send as (own mailbox + aliases
 * with send-as, `Me.identities`). Rendered only when there is a choice.
 */
import { useId } from 'react';
import type { Identity } from '@/api/types';
import { Icon } from '@/components/common';
import { t } from '@/i18n/zh';
import { displayAddress } from '@/lib/recipients';

export function identityLabel(i: Identity): string {
  return displayAddress({ name: i.display_name, email: i.email });
}

export interface IdentitySelectProps {
  identities: readonly Identity[];
  value: number | undefined;
  onChange: (addressId: number) => void;
  disabled?: boolean;
}

export function IdentitySelect({ identities, value, onChange, disabled }: IdentitySelectProps) {
  const id = useId();
  if (identities.length < 2) return null;
  const current = value ?? identities.find((i) => i.is_default)?.address_id ?? identities[0]?.address_id;
  return (
    <div className="cw-field">
      <label htmlFor={id} className="cw-field-label">
        {t('compose.from')}
      </label>
      <div className="relative flex min-w-0 flex-1 items-center">
        <select
          id={id}
          className="cw-from-select"
          value={current === undefined ? '' : String(current)}
          disabled={disabled}
          onChange={(e) => onChange(Number(e.target.value))}
        >
          {identities.map((i) => (
            <option key={i.address_id} value={String(i.address_id)}>
              {identityLabel(i)}
            </option>
          ))}
        </select>
        <Icon name="arrow_drop_down" size={20} className="pointer-events-none -ml-6 text-on-surface-variant" />
      </div>
    </div>
  );
}
