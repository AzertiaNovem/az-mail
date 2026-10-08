/**
 * F13: the compose window (TipTap / ProseMirror) is a lazily loaded chunk. The dock renders a
 * working window (title bar, close) before that module has loaded, and the form once it has.
 */
import { act, fireEvent, screen, waitFor } from '@testing-library/react';
import { describe, expect, it, vi } from 'vitest';
import { useComposeStore } from '@/stores/compose';
import { ComposeDock, preloadComposeWindow } from './ComposeDock';
import { installEditorDomPolyfills, makeQueryClient, renderWithProviders } from './testFixtures';

installEditorDomPolyfills();

const gate = vi.hoisted(() => {
  let release!: () => void;
  const promise = new Promise<void>((resolve) => {
    release = resolve;
  });
  return { promise, release: () => release() };
});

// The editor module only resolves when the test says so (a slow chunk download).
vi.mock('./ComposeWindow', async (importOriginal) => {
  await gate.promise;
  return importOriginal<typeof import('./ComposeWindow')>();
});

describe('ComposeDock lazy editor chunk', () => {
  it('shows a closable loading window until the compose module arrives, then the form', async () => {
    useComposeStore.setState({ windows: [], focusOrder: [], focusedKey: null, ownerId: null });
    renderWithProviders(<ComposeDock />, { qc: makeQueryClient() });
    act(() => void useComposeStore.getState().open({ kind: 'new', subject: '周报' }));

    // Not loaded yet: title bar + spinner, no editor.
    expect(screen.getByTestId('compose-window')).toBeInTheDocument();
    expect(screen.getByRole('heading', { name: '周报' })).toBeInTheDocument();
    expect(screen.getByText('正在加载…')).toBeInTheDocument();
    expect(document.querySelector('.ProseMirror')).toBeNull();
    expect(screen.queryByRole('textbox', { name: '主题' })).not.toBeInTheDocument();

    gate.release();
    await act(() => preloadComposeWindow().then(() => undefined));
    expect(await screen.findByRole('textbox', { name: '主题' })).toHaveValue('周报');
    expect(document.querySelector('.ProseMirror')).not.toBeNull();

    // Later windows open with the module already loaded: the form renders right away.
    act(() => void useComposeStore.getState().open({ kind: 'new', subject: '第二封' }));
    expect(screen.getAllByRole('textbox', { name: '主题' }).map((i) => (i as HTMLInputElement).value)).toEqual(['周报', '第二封']);
    expect(screen.queryByText('正在加载…')).not.toBeInTheDocument();
    fireEvent.click(screen.getAllByRole('button', { name: '保存并关闭' })[0]!);
    await waitFor(() => expect(screen.getAllByTestId('compose-window')).toHaveLength(1));
  });
});
