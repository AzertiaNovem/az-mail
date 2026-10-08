import { renderHook } from '@testing-library/react';
import { afterEach, describe, expect, it, vi } from 'vitest';
import {
  createKeydownHandler,
  dispatchShortcut,
  installShortcutListener,
  isEditableTarget,
  isImeKeyEvent,
  normalizeKey,
  registerShortcuts,
  SHORTCUT_HELP,
  shouldIgnoreKeyEvent,
  useShortcuts,
  type ShortcutAction,
} from './keyboard';

function keydown(target: EventTarget, key: string, init: KeyboardEventInit = {}): KeyboardEvent {
  const e = new KeyboardEvent('keydown', { key, bubbles: true, cancelable: true, ...init });
  target.dispatchEvent(e);
  return e;
}

function mount(html: string): HTMLElement {
  const host = document.createElement('div');
  host.innerHTML = html;
  document.body.appendChild(host);
  return host;
}

afterEach(() => {
  document.body.innerHTML = '';
});

describe('guards', () => {
  it('treats text inputs, textareas, selects and contenteditable as typing targets', () => {
    const host = mount(`
      <input id="t" type="text"><input id="e" type="email"><input id="c" type="checkbox">
      <textarea id="ta"></textarea><select id="s"></select>
      <div contenteditable="true"><p id="p">x</p></div><div contenteditable="false"><span id="ro">y</span></div>
      <button id="b">b</button>`);
    const $ = (id: string) => host.querySelector(`#${id}`);
    expect(isEditableTarget($('t'))).toBe(true);
    expect(isEditableTarget($('e'))).toBe(true);
    expect(isEditableTarget($('c'))).toBe(false);
    expect(isEditableTarget($('ta'))).toBe(true);
    expect(isEditableTarget($('s'))).toBe(true);
    expect(isEditableTarget($('p'))).toBe(true);
    expect(isEditableTarget($('ro'))).toBe(false);
    expect(isEditableTarget($('b'))).toBe(false);
    expect(isEditableTarget(null)).toBe(false);
  });

  it('ignores modifier chords, IME composition, handled events and blocking containers', () => {
    const host = mount(`<div role="dialog"><span id="in-dialog">x</span></div><div class="compose-dock"><span id="in-dock">y</span></div><span id="free">z</span><button id="btn">b</button>`);
    const free = host.querySelector('#free')!;
    const ev = (key: string, init: KeyboardEventInit = {}, target: Element = free) => {
      const e = new KeyboardEvent('keydown', { key, bubbles: true, cancelable: true, ...init });
      Object.defineProperty(e, 'target', { value: target });
      return e;
    };
    expect(shouldIgnoreKeyEvent(ev('e'))).toBe(false);
    expect(shouldIgnoreKeyEvent(ev('c', { ctrlKey: true }))).toBe(true);
    expect(shouldIgnoreKeyEvent(ev('c', { metaKey: true }))).toBe(true);
    expect(shouldIgnoreKeyEvent(ev('c', { altKey: true }))).toBe(true);
    expect(shouldIgnoreKeyEvent(ev('c', { isComposing: true }))).toBe(true);
    expect(shouldIgnoreKeyEvent(ev('e', {}, host.querySelector('#in-dialog')!))).toBe(true);
    expect(shouldIgnoreKeyEvent(ev('e', {}, host.querySelector('#in-dock')!))).toBe(true);
    // Enter on a focused button keeps its native meaning; letters still work there.
    expect(shouldIgnoreKeyEvent(ev('Enter', {}, host.querySelector('#btn')!))).toBe(true);
    expect(shouldIgnoreKeyEvent(ev('e', {}, host.querySelector('#btn')!))).toBe(false);
    const handled = ev('e');
    handled.preventDefault();
    expect(shouldIgnoreKeyEvent(handled)).toBe(true);
  });

  it('normalizes keys (case, Shift, CapsLock, symbols)', () => {
    expect(normalizeKey({ key: 'E', shiftKey: false })).toBe('e'); // CapsLock
    expect(normalizeKey({ key: 'I', shiftKey: true })).toBe('I');
    expect(normalizeKey({ key: 'i', shiftKey: true })).toBe('I');
    expect(normalizeKey({ key: '#', shiftKey: true })).toBe('#');
    expect(normalizeKey({ key: 'Enter', shiftKey: false })).toBe('Enter');
    expect(normalizeKey({ key: 'ArrowDown', shiftKey: false })).toBeNull();
  });
});

describe('dispatch', () => {
  it('maps keys to actions and prevents default only when handled', () => {
    const seen: ShortcutAction[] = [];
    const handler = createKeydownHandler({ dispatch: (a) => (seen.push(a), a !== 'help') });
    const body = document.body;
    const map: [string, KeyboardEventInit, ShortcutAction][] = [
      ['c', {}, 'compose'],
      ['/', {}, 'search'],
      ['j', {}, 'next'],
      ['k', {}, 'prev'],
      ['o', {}, 'open'],
      ['Enter', {}, 'open'],
      ['u', {}, 'back'],
      ['e', {}, 'archive'],
      ['#', { shiftKey: true }, 'delete'],
      ['!', { shiftKey: true }, 'spam'],
      ['s', {}, 'star'],
      ['x', {}, 'select'],
      ['I', { shiftKey: true }, 'markRead'],
      ['U', { shiftKey: true }, 'markUnread'],
      ['r', {}, 'reply'],
      ['a', {}, 'replyAll'],
      ['f', {}, 'forward'],
    ];
    for (const [key, init, action] of map) {
      const e = new KeyboardEvent('keydown', { key, bubbles: true, cancelable: true, ...init });
      Object.defineProperty(e, 'target', { value: body });
      handler(e);
      expect(seen.at(-1)).toBe(action);
      expect(e.defaultPrevented).toBe(true);
    }
    const help = new KeyboardEvent('keydown', { key: '?', shiftKey: true, cancelable: true });
    Object.defineProperty(help, 'target', { value: body });
    handler(help);
    expect(seen.at(-1)).toBe('help');
    expect(help.defaultPrevented).toBe(false); // nobody handled it
  });

  it('supports g-sequences within the timeout', () => {
    let now = 0;
    const seen: ShortcutAction[] = [];
    const handler = createKeydownHandler({ now: () => now, sequenceTimeoutMs: 1000, dispatch: (a) => (seen.push(a), true) });
    const press = (key: string) => {
      const e = new KeyboardEvent('keydown', { key, cancelable: true });
      Object.defineProperty(e, 'target', { value: document.body });
      handler(e);
    };
    press('g');
    press('i');
    press('g');
    press('t');
    press('g');
    now = 5000;
    press('a'); // too late: plain "a" (reply all)
    press('g');
    press('z'); // unknown second key: swallowed
    expect(seen).toEqual(['goInbox', 'goSent', 'replyAll']);
  });

  it('typing in an input never triggers shortcuts (installed listener)', () => {
    const fn = vi.fn();
    const off = registerShortcuts(() => ({ archive: fn }));
    const uninstall = installShortcutListener(window);
    const host = mount('<input id="i"><div id="d" tabindex="0"></div>');
    keydown(host.querySelector('#i')!, 'e');
    expect(fn).not.toHaveBeenCalled();
    keydown(host.querySelector('#d')!, 'e');
    expect(fn).toHaveBeenCalledOnce();
    uninstall();
    keydown(host.querySelector('#d')!, 'e');
    expect(fn).toHaveBeenCalledOnce();
    off();
  });

  it('the latest registered layer wins; lower layers handle what it does not', () => {
    const base = { archive: vi.fn(), compose: vi.fn() };
    const top = { archive: vi.fn() };
    const offBase = registerShortcuts(() => base);
    const offTop = registerShortcuts(() => top);
    expect(dispatchShortcut('archive')).toBe(true);
    expect(top.archive).toHaveBeenCalledOnce();
    expect(base.archive).not.toHaveBeenCalled();
    expect(dispatchShortcut('compose')).toBe(true);
    expect(base.compose).toHaveBeenCalledOnce();
    offTop();
    dispatchShortcut('archive');
    expect(base.archive).toHaveBeenCalledOnce();
    offBase();
    expect(dispatchShortcut('archive')).toBe(false);
  });

  it('useShortcuts registers while mounted and enabled, with the latest handlers', () => {
    const first = vi.fn();
    const second = vi.fn();
    const { rerender, unmount } = renderHook(({ fn, on }) => useShortcuts({ star: fn }, on), {
      initialProps: { fn: first, on: true },
    });
    dispatchShortcut('star');
    expect(first).toHaveBeenCalledOnce();
    rerender({ fn: second, on: true });
    dispatchShortcut('star');
    expect(second).toHaveBeenCalledOnce();
    rerender({ fn: second, on: false });
    expect(dispatchShortcut('star')).toBe(false);
    rerender({ fn: second, on: true });
    unmount();
    expect(dispatchShortcut('star')).toBe(false);
  });

  it('documents every shortcut in the help table', () => {
    expect(SHORTCUT_HELP.length).toBeGreaterThanOrEqual(20);
    expect(SHORTCUT_HELP.find((s) => s.label === 'mail.shortcuts.items.open')?.join).toBe('or');
  });
});

describe('review fixes', () => {
  it('F7: a listener on a same-origin iframe window handles keys typed while focus is in the frame', () => {
    const frame = document.createElement('iframe');
    document.body.appendChild(frame);
    const frameWin = frame.contentWindow! as Window & typeof globalThis;
    const doc = frame.contentDocument!;
    doc.body.innerHTML = '<p id="p">正文</p><input id="i">';
    const reply = vi.fn();
    const off = registerShortcuts(() => ({ reply }));
    const uninstall = installShortcutListener(frameWin);
    const send = (target: EventTarget, key: string) =>
      target.dispatchEvent(new frameWin.KeyboardEvent('keydown', { key, bubbles: true, cancelable: true }));
    send(doc.getElementById('p')!, 'r');
    expect(reply).toHaveBeenCalledOnce();
    // Guards still apply to the frame's own elements (cross-realm: not `instanceof` the app's Element).
    send(doc.getElementById('i')!, 'r');
    expect(reply).toHaveBeenCalledOnce();
    uninstall();
    send(doc.getElementById('p')!, 'r');
    expect(reply).toHaveBeenCalledOnce();
    off();
  });

  it('F8: recognizes IME composition keys from DOM and React events', () => {
    expect(isImeKeyEvent({ isComposing: true })).toBe(true);
    expect(isImeKeyEvent({ keyCode: 229 })).toBe(true);
    expect(isImeKeyEvent({ nativeEvent: { isComposing: true } })).toBe(true);
    expect(isImeKeyEvent({ nativeEvent: { keyCode: 229 } })).toBe(true);
    expect(isImeKeyEvent({ isComposing: false, keyCode: 13 })).toBe(false);
  });
});

