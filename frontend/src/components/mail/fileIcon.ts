/** File-type icon and accent color for attachments [WP-E] (Material Symbols names). */

export interface FileIcon {
  icon: string;
  color: string;
  /** Short type label ("PDF", "图片", …). */
  kind: string;
}

const EXT = (filename: string) => {
  const i = filename.lastIndexOf('.');
  return i > 0 ? filename.slice(i + 1).toLowerCase() : '';
};

export function isImageType(contentType: string): boolean {
  return /^image\/(png|jpe?g|gif|webp|avif|bmp)$/i.test(contentType.trim());
}

export function fileIconFor(contentType: string, filename: string): FileIcon {
  const ct = contentType.toLowerCase();
  const ext = EXT(filename);
  if (ct.startsWith('image/')) return { icon: 'image', color: '#d93025', kind: '图片' };
  if (ct === 'application/pdf' || ext === 'pdf') return { icon: 'picture_as_pdf', color: '#d93025', kind: 'PDF' };
  if (/(msword|wordprocessingml)/.test(ct) || ['doc', 'docx', 'odt', 'rtf'].includes(ext))
    return { icon: 'description', color: '#1a73e8', kind: 'Word' };
  if (/(ms-excel|spreadsheetml|csv)/.test(ct) || ['xls', 'xlsx', 'ods', 'csv'].includes(ext))
    return { icon: 'table_chart', color: '#188038', kind: 'Excel' };
  if (/(ms-powerpoint|presentationml)/.test(ct) || ['ppt', 'pptx', 'odp', 'key'].includes(ext))
    return { icon: 'slideshow', color: '#e37400', kind: 'PPT' };
  if (/(zip|rar|7z|tar|gzip|compressed)/.test(ct) || ['zip', 'rar', '7z', 'tar', 'gz', 'tgz'].includes(ext))
    return { icon: 'folder_zip', color: '#5f6368', kind: '压缩包' };
  if (ct.startsWith('audio/')) return { icon: 'audio_file', color: '#9334e6', kind: '音频' };
  if (ct.startsWith('video/')) return { icon: 'video_file', color: '#d93025', kind: '视频' };
  if (ct.startsWith('text/') || ['txt', 'md', 'log', 'json', 'xml', 'html', 'eml'].includes(ext))
    return { icon: 'article', color: '#5f6368', kind: '文本' };
  return { icon: 'draft', color: '#5f6368', kind: ext ? ext.toUpperCase() : '文件' };
}
