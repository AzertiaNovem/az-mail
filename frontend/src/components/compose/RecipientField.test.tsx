import { QueryClientProvider } from '@tanstack/react-query';
import { act, fireEvent, render, screen, waitFor } from '@testing-library/react';
import { createRef, useState } from 'react';
import { describe, expect, it, vi } from 'vitest';
import type { Address, Contact } from '@/api/types';
import { RecipientField, sortContacts, type RecipientFieldHandle } from './RecipientField';
import { makeQueryClient } from './testFixtures';

vi.mock('@/api/endpoints', async (importOriginal) => ({
  ...(await importOriginal<typeof import('@/api/endpoints')>()),
  searchContacts: vi.fn(async (q: string) => ({
    items: [
      { name: '外部', email: `ext-${q}@foo.com`, kind: 'contact' },
      { name: '李四', email: 'lisi@az.test', kind: 'team' },
      { name: '客服', email: 'support@az.test', kind: 'alias' },
      { name: '王五', email: 'wangwu@az.test', kind: 'team' },
    ] satisfies Contact[],
  })),
}));

function Harness({ initial = [] as Address[], onChange, flagged }: { initial?: Address[]; onChange?: (v: Address[]) => void; flagged?: Set<string> }) {
  const [value, setValue] = useState(initial);
  return (
    <QueryClientProvider client={makeQueryClient()}>
      <RecipientField
        label="收件人"
        value={value}
        flagged={flagged}
        debounceMs={0}
        onChange={(v) => {
          setValue(v);
          onChange?.(v);
        }}
      />
    </QueryClientProvider>
  );
}

const input = () => screen.getByRole('combobox', { name: '收件人' });
const chips = () => Array.from(document.querySelectorAll('.cw-chip > .truncate')).map((c) => c.textContent);

describe('RecipientField', () => {
  it('orders team members first, then aliases, then contacts', () => {
    const list: Contact[] = [
      { name: 'c', email: 'c@x', kind: 'contact' },
      { name: 'a1', email: 'a1@x', kind: 'alias' },
      { name: 't1', email: 't1@x', kind: 'team' },
      { name: 't2', email: 't2@x', kind: 'team' },
    ];
    expect(sortContacts(list).map((c) => c.name)).toEqual(['t1', 't2', 'a1', 'c']);
  });

  it('turns typed text into chips on separators, Enter and blur', () => {
    const onChange = vi.fn();
    render(<Harness onChange={onChange} />);
    fireEvent.change(input(), { target: { value: 'a@b.com,' } });
    expect(chips()).toEqual(['a@b.com']);
    expect(input()).toHaveValue('');
    fireEvent.change(input(), { target: { value: 'c@d.com；e' } });
    expect(chips()).toEqual(['a@b.com', 'c@d.com']);
    expect(input()).toHaveValue('e');
    fireEvent.change(input(), { target: { value: 'e@f.com' } });
    fireEvent.keyDown(input(), { key: 'Enter' });
    expect(chips()).toEqual(['a@b.com', 'c@d.com', 'e@f.com']);
    fireEvent.change(input(), { target: { value: 'g@h.com' } });
    fireEvent.blur(input());
    expect(chips()).toHaveLength(4);
    expect(onChange).toHaveBeenLastCalledWith([
      { name: '', email: 'a@b.com' },
      { name: '', email: 'c@d.com' },
      { name: '', email: 'e@f.com' },
      { name: '', email: 'g@h.com' },
    ]);
  });

  it('parses a pasted list and marks invalid addresses red', () => {
    render(<Harness />);
    const paste = new Event('paste', { bubbles: true, cancelable: true }) as Event & { clipboardData: unknown };
    paste.clipboardData = { getData: () => 'a@b.com, 张三 <c@d.com>, oops' };
    act(() => {
      input().dispatchEvent(paste);
    });
    expect(chips()).toEqual(['a@b.com', '张三', 'oops']);
    const bad = document.querySelectorAll('.cw-chip.is-invalid');
    expect(bad).toHaveLength(1);
    expect(bad[0]).toHaveAttribute('title', expect.stringContaining('此地址格式无效'));
  });

  it('marks addresses flagged by the server (unknown_local_recipient)', () => {
    render(<Harness initial={[{ name: '', email: 'Ghost@az.test' }, { name: '', email: 'ok@az.test' }]} flagged={new Set(['ghost@az.test'])} />);
    const bad = document.querySelectorAll('.cw-chip.is-invalid');
    expect(bad).toHaveLength(1);
    expect(bad[0]).toHaveAttribute('title', expect.stringContaining('此地址不存在'));
  });

  it('Backspace in an empty input removes the last chip; the × removes any chip', () => {
    render(<Harness initial={[{ name: 'A', email: 'a@x.com' }, { name: 'B', email: 'b@x.com' }, { name: 'C', email: 'c@x.com' }]} />);
    fireEvent.keyDown(input(), { key: 'Backspace' });
    expect(chips()).toEqual(['A', 'B']);
    fireEvent.click(screen.getByRole('button', { name: '移除 A' }));
    expect(chips()).toEqual(['B']);
  });

  it('double-click puts a chip back into the input for editing', () => {
    render(<Harness initial={[{ name: '张三', email: 'zs@x.com' }]} />);
    fireEvent.doubleClick(document.querySelector('.cw-chip')!);
    expect(chips()).toEqual([]);
    expect(input()).toHaveValue('张三 <zs@x.com>');
  });

  it('suggests contacts (team first, already-added excluded) with keyboard navigation', async () => {
    render(<Harness initial={[{ name: '', email: 'wangwu@az.test' }]} />);
    fireEvent.focus(input());
    fireEvent.change(input(), { target: { value: 'li' } });
    const listbox = await screen.findByRole('listbox');
    const options = () => Array.from(listbox.querySelectorAll('[role=option]')).map((o) => o.textContent);
    await waitFor(() => expect(options()).toHaveLength(3));
    expect(options()[0]).toContain('李四');
    expect(options()[1]).toContain('客服');
    expect(options()[2]).toContain('ext-li@foo.com');
    expect(input()).toHaveAttribute('aria-expanded', 'true');

    expect(listbox.querySelector('[aria-selected=true]')?.textContent).toContain('李四');
    fireEvent.keyDown(input(), { key: 'ArrowDown' });
    expect(input()).toHaveAttribute('aria-activedescendant', expect.stringMatching(/-1$/));
    fireEvent.keyDown(input(), { key: 'ArrowUp' });
    fireEvent.keyDown(input(), { key: 'ArrowUp' }); // wraps to the last
    expect(listbox.querySelector('[aria-selected=true]')?.textContent).toContain('ext-li@foo.com');
    fireEvent.keyDown(input(), { key: 'ArrowDown' }); // back to the first
    fireEvent.keyDown(input(), { key: 'Enter' });
    expect(chips()).toEqual(['wangwu@az.test', '李四']);
    expect(screen.queryByRole('listbox')).not.toBeInTheDocument();
  });

  it('Escape closes suggestions without bubbling a close to the window', async () => {
    const onWindowKey = vi.fn();
    render(
      // eslint-disable-next-line jsx-a11y/no-static-element-interactions
      <div onKeyDown={(e) => onWindowKey(e.defaultPrevented)}>
        <Harness />
      </div>,
    );
    fireEvent.focus(input());
    fireEvent.change(input(), { target: { value: 'li' } });
    await screen.findByRole('listbox');
    fireEvent.keyDown(input(), { key: 'Escape' });
    expect(screen.queryByRole('listbox')).not.toBeInTheDocument();
    expect(onWindowKey).toHaveBeenCalledWith(true);
  });

  it('commit() exposes pending text synchronously (Ctrl+Enter / send)', () => {
    const ref = createRef<RecipientFieldHandle>();
    let latest: Address[] = [];
    function Imperative() {
      const [value, setValue] = useState<Address[]>([]);
      return (
        <QueryClientProvider client={makeQueryClient()}>
          <RecipientField
            ref={ref}
            label="收件人"
            value={value}
            onChange={(v) => {
              latest = v;
              setValue(v);
            }}
          />
        </QueryClientProvider>
      );
    }
    render(<Imperative />);
    fireEvent.change(input(), { target: { value: 'x@y.com' } });
    let result: Address[] = [];
    act(() => {
      result = ref.current!.commit();
    });
    expect(result).toEqual([{ name: '', email: 'x@y.com' }]);
    expect(latest).toEqual(result);
  });
});
