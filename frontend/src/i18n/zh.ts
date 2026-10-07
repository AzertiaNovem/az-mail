/**
 * Simplified Chinese UI strings. All user-visible text goes through `t()` so wording stays
 * consistent across work packages. Placeholders use `{name}` and are filled from `vars`.
 *
 * Ownership (one owner per file, so E and F never edit the same file):
 * - this file [WP0, frozen]: shared strings (folders, statuses, actions, common, toasts, errors);
 * - zh.mail.ts [E]: `mail.*` keys and extra error-code messages (`mailErrors`);
 * - zh.compose.ts [F]: `compose.*`, `settings.*`, `admin.*` keys and `composeErrors`.
 * All of them are merged into `zh` below, so every key stays typed (`t('compose.discardConfirm')`).
 * Package files add keys only; nobody renames or removes an existing key.
 */
import type { FolderId, OutboundStatus, ThreadAction } from '@/api/types';
import { admin, compose, composeErrors, settings } from './zh.compose';
import { mail, mailErrors } from './zh.mail';

const folders = {
  inbox: '收件箱',
  starred: '已加星标',
  scheduled: '已定时',
  sent: '已发送',
  drafts: '草稿',
  all: '所有邮件',
  spam: '垃圾邮件',
  trash: '已删除',
} as const satisfies Record<FolderId, string>;

const status = {
  queued: '待发送',
  sending: '发送中',
  accepted: '已发送',
  scheduled: '已定时',
  sent: '已发送',
  delivered: '已送达',
  delivery_delayed: '投递延迟',
  bounced: '退信',
  complained: '被标记为垃圾邮件',
  failed: '发送失败',
  suppressed: '已被抑制',
  canceled: '已取消',
} as const satisfies Record<OutboundStatus, string>;

/** Labels for thread-action buttons / menu items. */
const threadActions = {
  archive: '归档',
  inbox: '移至收件箱',
  read: '标记为已读',
  unread: '标记为未读',
  star: '加星标',
  unstar: '取消星标',
  trash: '删除',
  restore: '恢复',
  spam: '举报垃圾邮件',
  not_spam: '不是垃圾邮件',
  delete_forever: '永久删除',
  add_label: '添加标签',
  remove_label: '移除标签',
} as const satisfies Record<ThreadAction, string>;

/** Messages per error code (and a few generic ones); see KnownErrorCode in api/types.ts. */
const errors = {
  network: '网络连接失败，请检查网络后重试',
  unknown: '发生未知错误，请稍后重试',
  server: '服务器出错，请稍后重试',
  unauthorized: '登录已过期，请重新登录',
  forbidden: '没有权限执行此操作',
  not_found: '内容不存在或已被删除',
  payload_too_large: '文件过大（单个附件最大 25 MB）',
  too_many_attempts: '尝试次数过多，请稍后再试',
  invalid_response: '服务器返回了无法识别的数据',
  invalid_credentials: '邮箱或密码错误',
  version_conflict: '草稿已在其他窗口修改',
  too_late: '已无法撤销，邮件已发出',
  already_sent: '邮件已发出，无法取消定时',
  send_as_forbidden: '你没有权限以此地址发送邮件',
  unknown_local_recipient: '收件人地址不存在',
  too_many_recipients: '收件人过多（每栏最多 50 个）',
  invalid_schedule: '定时时间无效（需在 1 分钟后至 30 天内）',
  message_too_large: '邮件过大（附件总计最大 28 MB）',
  // errors.hpp factory defaults / core/json (shown only when the server sends no message)
  bad_request: '请求无效',
  invalid_json: '请求格式无效',
  invalid_field: '请求内容有误',
  conflict: '操作冲突，请刷新后重试',
  unprocessable: '无法处理该请求',
  too_many_requests: '请求过于频繁，请稍后再试',
  internal_error: '服务器内部错误，请稍后重试',
  bad_gateway: '上游服务出错，请稍后重试',
  service_unavailable: '服务繁忙，请稍后再试',
  resend_error: '邮件服务暂时不可用，请稍后重试',
} as const;

export const zh = {
  app: {
    name: 'AZ Mail',
    loading: '正在加载…',
    notFound: '找不到该页面',
    backHome: '返回收件箱',
  },
  folders,
  status: {
    ...status,
    /** "已定时 {time}" */
    scheduledAt: '已定时 {time}',
    unknown: '未知状态',
  },
  threadActions,
  actions: {
    compose: '写邮件',
    send: '发送',
    scheduleSend: '定时发送',
    undo: '撤销',
    viewMessage: '查看邮件',
    reply: '回复',
    replyAll: '全部回复',
    forward: '转发',
    archive: '归档',
    delete: '删除',
    deleteForever: '永久删除',
    restore: '恢复',
    moveTo: '移至',
    labelAs: '添加标签',
    markRead: '标记为已读',
    markUnread: '标记为未读',
    star: '加星标',
    unstar: '取消星标',
    reportSpam: '举报垃圾邮件',
    notSpam: '不是垃圾邮件',
    refresh: '刷新',
    more: '更多',
    search: '搜索邮件',
    clearSearch: '清除搜索',
    showSearchOptions: '显示搜索选项',
    selectAll: '全选',
    newer: '较新',
    older: '较旧',
    back: '返回',
    close: '关闭',
    cancel: '取消',
    confirm: '确定',
    save: '保存',
    edit: '修改',
    create: '创建',
    remove: '移除',
    retry: '重试',
    download: '下载',
    preview: '预览',
    print: '打印',
    showOriginal: '显示原始邮件',
    attach: '添加附件',
    insertImage: '插入图片',
    discardDraft: '舍弃草稿',
    minimize: '最小化',
    maximize: '全屏',
    exitMaximize: '退出全屏',
    settings: '设置',
    admin: '管理后台',
    logout: '退出登录',
    login: '登录',
    showImages: '显示图片',
    alwaysShowImagesFrom: '始终显示来自 {sender} 的图片',
    reload: '重新加载',
    overwrite: '覆盖',
    newLabel: '新建标签',
    mainMenu: '主菜单',
  },
  common: {
    me: '我',
    to: '收件人',
    cc: '抄送',
    bcc: '密送',
    from: '发件人',
    subject: '主题',
    noSubject: '(无主题)',
    draft: '草稿',
    drafts: '草稿',
    labels: '标签',
    attachments: '附件',
    attachmentCount: '{count} 个附件',
    bccToMe: '密送给我',
    sentBy: '由 {name} 发送',
    newMessage: '新邮件',
    today: '今天',
    yesterday: '昨天',
    empty: '没有邮件',
    pageRange: '第 {from}–{to} 封，共 {total} 封',
    pageRangeOpen: '第 {from}–{to} 封',
    selectedCount: '已选择 {count} 个会话',
    saving: '正在保存…',
    saved: '已保存草稿',
    saveFailed: '保存失败',
    savedConflict: '已在其他窗口修改',
    uploading: '正在上传…',
    sending: '正在发送…',
    hiddenImages: '已隐藏外部图片',
    emptySubjectConfirm: '确定要发送没有主题的邮件吗？',
    yes: '是',
    no: '否',
    email: '邮箱',
    password: '密码',
    displayName: '显示名称',
  },
  toasts: {
    sent: '邮件已发送',
    scheduled: '邮件已定时发送',
    undone: '已撤销发送',
    archived: '会话已归档',
    trashed: '会话已移至已删除',
    deletedForever: '会话已永久删除',
    markedSpam: '会话已标记为垃圾邮件',
    movedToInbox: '会话已移至收件箱',
    markedRead: '已标记为已读',
    markedUnread: '已标记为未读',
    labelAdded: '已添加标签“{name}”',
    labelRemoved: '已移除标签“{name}”',
    actionFailed: '操作失败，请重试',
    copied: '已复制',
    sessionExpired: '登录已过期，请重新登录',
    loggedOutElsewhere: '你已在其他标签页退出登录',
  },
  errors: { ...errors, ...mailErrors, ...composeErrors },
  mail,
  compose,
  settings,
  admin,
} as const;

type Join<K, P> = K extends string ? (P extends string ? `${K}.${P}` : never) : never;

/** Dot paths to every string leaf of `zh`, e.g. 'folders.inbox'. */
export type MessageKey<T = typeof zh> = {
  [K in keyof T & string]: T[K] extends string ? K : Join<K, MessageKey<T[K]>>;
}[keyof T & string];

export type MessageVars = Record<string, string | number>;

function lookup(key: string): string | undefined {
  let node: unknown = zh;
  for (const part of key.split('.')) {
    if (typeof node !== 'object' || node === null) return undefined;
    node = (node as Record<string, unknown>)[part];
  }
  return typeof node === 'string' ? node : undefined;
}

/** Interpolates `{name}` placeholders. Unknown placeholders are left as-is. */
export function format(template: string, vars?: MessageVars): string {
  if (!vars) return template;
  return template.replace(/\{(\w+)\}/g, (m, name: string) => (name in vars ? String(vars[name]) : m));
}

/** Translates a key; falls back to the key itself if missing (never throws). */
export function t(key: MessageKey, vars?: MessageVars): string {
  return format(lookup(key) ?? key, vars);
}

/** Display name of a system folder. */
export const folderName = (folder: FolderId): string => zh.folders[folder];

/** Chinese label of an outbound status; `scheduledTime` renders "已定时 {time}". */
export function statusName(s: OutboundStatus, scheduledTime?: string): string {
  if (s === 'scheduled' && scheduledTime) return format(zh.status.scheduledAt, { time: scheduledTime });
  return zh.status[s];
}

/** Localized message for a known API error code, if any. */
export function errorCodeMessage(code: string): string | undefined {
  return (zh.errors as Record<string, string>)[code];
}
