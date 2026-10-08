/**
 * Sandboxed email body [WP-E] (DESIGN.md §5 steps 2–5, 8, 9; §1 D3).
 *
 * The HTML is sanitized (lib/sanitize.ts), wrapped in a srcdoc with its own CSP
 * (lib/emailFrame.ts) and shown in an iframe without `allow-scripts`. Same-origin access lets
 * this component size the frame to its content (ResizeObserver + image load events), turn
 * mailto: clicks into a compose window, and keep the Gmail shortcuts working while focus is
 * inside the frame (a click into the body moves keyboard focus into its browsing context, so
 * the app's window listener would never see the keys). The trailing quoted block starts
 * collapsed behind "…".
 */
import { useCallback, useEffect, useLayoutEffect, useMemo, useRef, useState } from 'react';
import { Icon, Tooltip } from '@/components/common';
import { t } from '@/i18n/zh';
import { buildSrcDoc, EMAIL_FRAME_SANDBOX, frameFilesOrigins, measureContentHeight, parseMailto, type MailtoParts } from '@/lib/emailFrame';
import { installShortcutListener } from '@/lib/keyboard';
import { sanitizeEmailHtml, type SanitizeResult } from '@/lib/sanitize';

export interface EmailContent {
  result: SanitizeResult;
  /** Origins allowed in the frame CSP (files origins + the API origin). */
  origins: string[];
  allowRemote: boolean;
}

/**
 * Sanitizes a message body once per (html, allowRemote, origins); the caller reads
 * `result.blockedImages` for the banner and passes the content to <EmailFrame>.
 */
export function useEmailContent(html: string, allowRemote: boolean, filesOrigins: readonly string[]): EmailContent {
  const origins = useMemo(() => frameFilesOrigins(filesOrigins), [filesOrigins]);
  const result = useMemo(() => sanitizeEmailHtml(html, { allowRemote, filesOrigins: origins, markQuotes: true }), [html, allowRemote, origins]);
  return useMemo(() => ({ result, origins, allowRemote }), [result, origins, allowRemote]);
}

export interface EmailFrameProps {
  content: EmailContent;
  title: string;
  onMailto?: (parts: MailtoParts) => void;
}

const MIN_HEIGHT = 24;

interface ClosestCapable {
  closest?: (selector: string) => { getAttribute(name: string): string | null } | null;
}

export function EmailFrame({ content, title, onMailto }: EmailFrameProps) {
  const { result, origins, allowRemote } = content;
  const [showQuote, setShowQuote] = useState(false);
  const collapse = !!result.hasQuote && !showQuote;
  const srcDoc = useMemo(
    () => buildSrcDoc(result.html, { allowRemote, filesOrigins: origins, collapseQuotes: collapse }),
    [result.html, allowRemote, origins, collapse],
  );
  const frameRef = useRef<HTMLIFrameElement>(null);
  const [height, setHeight] = useState(MIN_HEIGHT);
  const cleanupRef = useRef<(() => void) | null>(null);
  const mailtoRef = useRef(onMailto);
  useEffect(() => {
    mailtoRef.current = onMailto;
  });

  const attachedDoc = useRef<Document | null>(null);
  const measureRef = useRef<(() => void) | null>(null);

  /** Hooks the current frame document (once per document): sizing, image loads, mailto: clicks. */
  const attach = useCallback(() => {
    const frame = frameRef.current;
    const doc = frame?.contentDocument;
    if (!frame || !doc || !doc.documentElement || doc.readyState === 'loading') return;
    if (doc === attachedDoc.current) {
      measureRef.current?.(); // e.g. `load` after the images arrived
      return;
    }
    cleanupRef.current?.();
    cleanupRef.current = null;
    attachedDoc.current = doc;

    let raf = 0;
    const measure = () => {
      cancelAnimationFrame(raf);
      raf = requestAnimationFrame(() => {
        // A replaced (or detached) document must not resize the frame.
        if (frame.contentDocument !== doc) return;
        const h = measureContentHeight(doc);
        if (h > 0) setHeight(Math.max(MIN_HEIGHT, h));
      });
    };
    measureRef.current = measure;
    measure();

    const ro = typeof ResizeObserver !== 'undefined' ? new ResizeObserver(measure) : null;
    ro?.observe(doc.documentElement);
    if (doc.body) ro?.observe(doc.body);
    const imgs = Array.from(doc.images);
    for (const img of imgs) {
      img.addEventListener('load', measure);
      img.addEventListener('error', measure);
    }
    const onClick = (e: MouseEvent) => {
      const target = e.target as ClosestCapable | null;
      const a = target && typeof target.closest === 'function' ? target.closest('a[href]') : null;
      const href = a?.getAttribute('href') ?? '';
      if (!/^\s*mailto:/i.test(href)) return;
      e.preventDefault();
      const parts = parseMailto(href);
      if (parts) mailtoRef.current?.(parts);
    };
    doc.addEventListener('click', onClick);
    // Keys typed while the frame has focus go to the frame's window: dispatch them as shortcuts too.
    const frameWin = frame.contentWindow;
    const stopKeys = frameWin ? installShortcutListener(frameWin) : null;
    const win = frame.ownerDocument.defaultView;
    win?.addEventListener('resize', measure);
    // Late layout changes (web fonts, slow images without load events) settle within a few seconds.
    const timers = [250, 1000, 3000].map((ms) => setTimeout(measure, ms));

    cleanupRef.current = () => {
      cancelAnimationFrame(raf);
      ro?.disconnect();
      for (const img of imgs) {
        img.removeEventListener('load', measure);
        img.removeEventListener('error', measure);
      }
      doc.removeEventListener('click', onClick);
      stopKeys?.();
      win?.removeEventListener('resize', measure);
      for (const id of timers) clearTimeout(id);
      measureRef.current = null;
    };
  }, []);

  // A new srcdoc replaces the document. Its `load` event waits for every image (a slow remote
  // one can take ages), so hook the new document as soon as it is parsed instead.
  useLayoutEffect(() => {
    const previous = frameRef.current?.contentDocument ?? null;
    let tries = 0;
    const id = setInterval(() => {
      const doc = frameRef.current?.contentDocument;
      if (doc && doc !== previous && doc.readyState !== 'loading') {
        clearInterval(id);
        attach();
      } else if (++tries > 200) clearInterval(id);
    }, 25);
    return () => clearInterval(id);
  }, [srcDoc, attach]);

  useEffect(
    () => () => {
      cleanupRef.current?.();
      attachedDoc.current = null;
    },
    [],
  );

  return (
    <div className="azm-email-frame">
      <iframe
        ref={frameRef}
        title={title}
        sandbox={EMAIL_FRAME_SANDBOX}
        referrerPolicy="no-referrer"
        srcDoc={srcDoc}
        onLoad={attach}
        scrolling="no"
        className="block w-full border-0 bg-white"
        style={{ height, colorScheme: 'light' }}
      />
      {result.hasQuote && (
        <Tooltip label={showQuote ? t('mail.thread.hideQuoted') : t('mail.thread.showQuoted')}>
          <button
            type="button"
            aria-label={showQuote ? t('mail.thread.hideQuoted') : t('mail.thread.showQuoted')}
            aria-expanded={showQuote}
            onClick={() => setShowQuote((v) => !v)}
            className="azm-quote-toggle mt-2"
          >
            <Icon name="more_horiz" size={18} />
          </button>
        </Tooltip>
      )}
    </div>
  );
}
