import { describe, expect, it } from 'vitest';
import { errorCodeMessage, folderName, format, statusName, t } from './zh';

describe('zh i18n', () => {
  it('translates keys and interpolates variables', () => {
    expect(t('folders.inbox')).toBe('收件箱');
    expect(t('common.attachmentCount', { count: 3 })).toBe('3 个附件');
    expect(format('{a} 和 {b}', { a: 1 })).toBe('1 和 {b}');
  });

  it('names folders and statuses', () => {
    expect(folderName('trash')).toBe('已删除');
    expect(folderName('all')).toBe('所有邮件');
    expect(statusName('complained')).toBe('被标记为垃圾邮件');
    expect(statusName('scheduled', '10月8日 09:00')).toBe('已定时 10月8日 09:00');
    expect(statusName('delivery_delayed')).toBe('投递延迟');
  });

  it('maps error codes', () => {
    expect(errorCodeMessage('too_late')).toBe('已无法撤销，邮件已发出');
    expect(errorCodeMessage('nope')).toBeUndefined();
  });
});
