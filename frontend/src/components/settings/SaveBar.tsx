/** Sticky "保存更改 / 取消" bar of a settings form [WP-F]. */
import { Button } from '@/components/common';
import { t } from '@/i18n/zh';

export interface SaveBarProps {
  dirty: boolean;
  saving: boolean;
  onSave: () => void;
  onCancel: () => void;
}

export function SaveBar({ dirty, saving, onSave, onCancel }: SaveBarProps) {
  return (
    <div className="azm-save-bar" role="group" aria-label={t('settings.saveBar.save')}>
      <Button onClick={onSave} disabled={!dirty} loading={saving}>
        {t('settings.saveBar.save')}
      </Button>
      <Button variant="outlined" onClick={onCancel} disabled={!dirty || saving}>
        {t('settings.saveBar.cancel')}
      </Button>
      {dirty && !saving && <span className="ml-2 text-xs text-on-surface-variant">{t('settings.saveBar.unsaved')}</span>}
    </div>
  );
}
