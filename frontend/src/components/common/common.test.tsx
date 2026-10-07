import { act, fireEvent, render, screen } from '@testing-library/react';
import { afterEach, describe, expect, it, vi } from 'vitest';
import { toast } from '@/stores/toast';
import { Avatar, avatarColor, avatarInitial, AVATAR_COLORS } from './Avatar';
import { Button } from './Button';
import { Checkbox } from './Checkbox';
import { ConfirmDialog } from './Dialog';
import { Icon } from './Icon';
import { IconButton } from './IconButton';
import { Popover } from './Popover';
import { Select } from './Select';
import { Switch } from './Switch';
import { TextField } from './TextField';
import { ToastHost } from './ToastHost';
import { TooltipProvider } from './Tooltip';

describe('common components', () => {
  afterEach(() => act(() => toast.clear()));

  it('Avatar: deterministic color and CJK-aware initial', () => {
    expect(avatarColor('Bob@Example.com')).toBe(avatarColor('bob@example.com'));
    expect(AVATAR_COLORS).toContain(avatarColor('x@y.z'));
    expect(avatarInitial('张三', 'zs@example.com')).toBe('张');
    expect(avatarInitial('', 'bob@example.com')).toBe('B');
    expect(avatarInitial('"alice"', 'a@example.com')).toBe('A');
    render(<Avatar name="李四" email="ls@example.com" />);
    expect(screen.getByRole('img', { name: '李四' })).toHaveTextContent('李');
  });

  it('Icon is decorative unless labelled', () => {
    const { container } = render(<Icon name="star" fill />);
    const span = container.querySelector('span')!;
    expect(span).toHaveAttribute('aria-hidden', 'true');
    expect(span.style.getPropertyValue('--icon-fill')).toBe('1');
    render(<Icon name="attach_file" label="附件" />);
    expect(screen.getByRole('img', { name: '附件' })).toBeInTheDocument();
  });

  it('IconButton exposes its label and pressed state, with or without a provider', () => {
    const onClick = vi.fn();
    render(<IconButton icon="star" label="加星标" active={false} onClick={onClick} />);
    const btn = screen.getByRole('button', { name: '加星标' });
    expect(btn).toHaveAttribute('aria-pressed', 'false');
    fireEvent.click(btn);
    expect(onClick).toHaveBeenCalledOnce();
    render(
      <TooltipProvider>
        <IconButton icon="archive" label="归档" />
      </TooltipProvider>,
    );
    expect(screen.getByRole('button', { name: '归档' })).toBeInTheDocument();
  });

  it('Button: loading disables it; the spinner stays out of the accessible name', () => {
    render(
      <Button loading icon="send">
        发送
      </Button>,
    );
    const btn = screen.getByRole('button', { name: '发送' });
    expect(btn).toBeDisabled();
    expect(btn).toHaveAttribute('aria-busy', 'true');
    expect(btn).toHaveAttribute('type', 'button');
    expect(screen.queryByRole('status')).not.toBeInTheDocument();
  });

  it('ConfirmDialog accepts block content as its message (rendered in a div, not a p)', () => {
    const errors = vi.spyOn(console, 'error').mockImplementation(() => {});
    render(
      <ConfirmDialog
        open
        onOpenChange={() => {}}
        title="永久删除"
        message={
          <ul>
            <li>会话 1</li>
          </ul>
        }
        onConfirm={() => {}}
      />,
    );
    const dialog = screen.getByRole('dialog', { name: '永久删除' });
    const list = screen.getByRole('list');
    expect(list.parentElement!.tagName).toBe('DIV');
    expect(dialog).toHaveAccessibleDescription('会话 1');
    expect(errors).not.toHaveBeenCalled();
  });

  it('TextField wires label, error and helper text for assistive tech', () => {
    const { rerender } = render(<TextField label="邮箱" helperText="例如 a@b.com" defaultValue="" />);
    const input = screen.getByRole('textbox', { name: '邮箱' });
    expect(input).not.toHaveAttribute('aria-invalid');
    expect(input).toHaveAccessibleDescription('例如 a@b.com');
    rerender(<TextField label="邮箱" helperText="例如 a@b.com" error="邮箱格式不正确" defaultValue="" />);
    expect(input).toHaveAttribute('aria-invalid', 'true');
    expect(input).toHaveAccessibleDescription('邮箱格式不正确');
    expect(screen.queryByText('例如 a@b.com')).not.toBeInTheDocument();
  });

  it('Select reports the chosen value', () => {
    const onValueChange = vi.fn();
    render(
      <Select
        label="撤销发送"
        value="5"
        onValueChange={onValueChange}
        options={[
          { value: '0', label: '关闭' },
          { value: '5', label: '5 秒' },
          { value: '10', label: '10 秒' },
        ]}
      />,
    );
    const select = screen.getByRole('combobox', { name: '撤销发送' });
    expect(select).toHaveValue('5');
    fireEvent.change(select, { target: { value: '10' } });
    expect(onValueChange).toHaveBeenCalledWith('10');
  });

  it('Switch is a role=switch button toggled by click or its label', () => {
    const onChange = vi.fn();
    const { rerender } = render(<Switch checked={false} onCheckedChange={onChange} label="启用签名" />);
    const sw = screen.getByRole('switch', { name: '启用签名' });
    expect(sw).toHaveAttribute('aria-checked', 'false');
    fireEvent.click(sw);
    expect(onChange).toHaveBeenLastCalledWith(true);
    rerender(<Switch checked onCheckedChange={onChange} label="启用签名" />);
    expect(sw).toHaveAttribute('aria-checked', 'true');
    fireEvent.click(screen.getByText('启用签名'));
    expect(onChange).toHaveBeenLastCalledWith(false);
  });

  it('Popover opens from its trigger and closes on Escape', () => {
    render(
      <Popover trigger={<button type="button">搜索选项</button>} aria-label="高级搜索">
        <p>发件人</p>
      </Popover>,
    );
    expect(screen.queryByText('发件人')).not.toBeInTheDocument();
    fireEvent.click(screen.getByRole('button', { name: '搜索选项' }));
    expect(screen.getByRole('dialog', { name: '高级搜索' })).toHaveTextContent('发件人');
    fireEvent.keyDown(screen.getByRole('dialog'), { key: 'Escape' });
    expect(screen.queryByText('发件人')).not.toBeInTheDocument();
  });

  it('Checkbox toggles through onCheckedChange', () => {
    const onChange = vi.fn();
    render(<Checkbox checked={false} onCheckedChange={onChange} label="全选" />);
    fireEvent.click(screen.getByRole('checkbox', { name: '全选' }));
    expect(onChange).toHaveBeenCalledWith(true);
  });

  it('ToastHost renders toasts with actions that dismiss', () => {
    const onUndo = vi.fn();
    render(<ToastHost />);
    act(() => {
      toast.push({ message: '邮件已发送', action: { label: '撤销', onClick: onUndo }, durationMs: Infinity });
    });
    expect(screen.getByText('邮件已发送')).toBeInTheDocument();
    fireEvent.click(screen.getByRole('button', { name: '撤销' }));
    expect(onUndo).toHaveBeenCalledOnce();
    expect(screen.queryByText('邮件已发送')).not.toBeInTheDocument();

    act(() => {
      toast.push({ message: '发送失败', tone: 'error', durationMs: Infinity });
    });
    expect(screen.getByRole('alert')).toHaveTextContent('发送失败');
    fireEvent.click(screen.getByRole('button', { name: '关闭' }));
    expect(screen.queryByRole('alert')).not.toBeInTheDocument();
  });
});
