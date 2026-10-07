/**
 * Quoted original under the editor [WP-F] (DESIGN.md §1 C10): a Gmail "…" button that reveals
 * `quoted_html` read-only. The preview renders in a sandboxed iframe (no scripts; same-origin
 * only so the parent can size it) with a restrictive meta CSP, so the quoted mail's styles can
 * neither leak into the app nor load anything but allowed images (§1 D3).
 */
import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { Button, Icon, Tooltip } from '@/components/common';
import { t } from '@/i18n/zh';
import { escapeHtml } from '@/lib/quote';
import { sanitizeEmailHtml } from '@/lib/sanitize';

/** srcdoc of the quote preview. */
export function buildQuoteSrcdoc(html: string, opts: { allowRemote: boolean; filesOrigins: string[] }): string {
  const { html: body } = sanitizeEmailHtml(html, opts);
  const imgSrc = ['data:', ...opts.filesOrigins, ...(opts.allowRemote ? ['https:', 'http:'] : [])].join(' ');
  const csp = `default-src 'none'; img-src ${imgSrc}; style-src 'unsafe-inline'; font-src data:`;
  return (
    `<!doctype html><html><head><meta charset="utf-8">` +
    `<meta http-equiv="Content-Security-Policy" content="${escapeHtml(csp)}">` +
    `<base target="_blank">` +
    `<style>html,body{margin:0;padding:0;background:#fff;color:#222;font:14px/1.5 "PingFang SC","Microsoft YaHei","Noto Sans SC",Arial,sans-serif;word-break:break-word;overflow-wrap:anywhere}` +
    `img{max-width:100%;height:auto}blockquote{margin:0 0 0 .8ex;border-left:1px solid #ccc;padding-left:1ex}a{color:#0b57d0}</style>` +
    `</head><body>${body}</body></html>`
  );
}

export interface QuotedToggleProps {
  html: string;
  expanded: boolean;
  onToggle: () => void;
  /** Drops the quote from the draft. */
  onRemove?: () => void;
  allowRemote: boolean;
  filesOrigins: string[];
}

export function QuotedToggle({ html, expanded, onToggle, onRemove, allowRemote, filesOrigins }: QuotedToggleProps) {
  const label = expanded ? t('compose.quoted.hide') : t('compose.quoted.show');
  return (
    <div className="cw-quote">
      <Tooltip label={label} side="top">
        <button type="button" className="cw-quote-toggle" aria-label={label} aria-expanded={expanded} onClick={onToggle}>
          <Icon name="more_horiz" size={20} />
        </button>
      </Tooltip>
      {expanded && (
        <div className="cw-quote-body">
          <QuoteFrame html={html} allowRemote={allowRemote} filesOrigins={filesOrigins} />
          {onRemove && (
            <Button variant="text" size="sm" icon="format_clear" onClick={onRemove} className="mt-1">
              {t('compose.quoted.remove')}
            </Button>
          )}
        </div>
      )}
    </div>
  );
}

function QuoteFrame({ html, allowRemote, filesOrigins }: { html: string; allowRemote: boolean; filesOrigins: string[] }) {
  const ref = useRef<HTMLIFrameElement>(null);
  const [height, setHeight] = useState(120);
  const srcdoc = useMemo(() => buildQuoteSrcdoc(html, { allowRemote, filesOrigins }), [html, allowRemote, filesOrigins]);

  const measure = useCallback(() => {
    const doc = ref.current?.contentDocument;
    if (!doc?.documentElement) return;
    setHeight(Math.max(40, Math.ceil(doc.documentElement.scrollHeight)));
  }, []);

  useEffect(() => {
    const frame = ref.current;
    if (!frame) return;
    let ro: ResizeObserver | undefined;
    const onLoad = () => {
      measure();
      const body = frame.contentDocument?.body;
      if (body && typeof ResizeObserver !== 'undefined') {
        ro?.disconnect();
        ro = new ResizeObserver(measure);
        ro.observe(body);
      }
    };
    frame.addEventListener('load', onLoad);
    return () => {
      frame.removeEventListener('load', onLoad);
      ro?.disconnect();
    };
  }, [measure, srcdoc]);

  return (
    <iframe
      ref={ref}
      title={t('compose.quoted.frameTitle')}
      className="cw-quote-frame"
      sandbox="allow-same-origin allow-popups allow-popups-to-escape-sandbox"
      referrerPolicy="no-referrer"
      srcDoc={srcdoc}
      style={{ height }}
    />
  );
}
