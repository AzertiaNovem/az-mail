/**
 * Compose / settings / admin UI strings [WP-F]. Only WP-F edits this file; it is merged into
 * `zh` by zh.ts, so keys stay typed: `t('compose.discardConfirm')`, `t('settings.tabs.general')`,
 * `t('admin.users.create')`.
 *
 * - `compose`: compose windows, recipients, editor, attachments, schedule picker.
 * - `settings`: the settings page tabs.
 * - `admin`: the admin pages.
 * - `composeErrors`: messages for API error codes not already in `zh.errors` (merged into
 *   `zh.errors`; add new codes only, never redefine an existing one).
 */
export const compose = {} as const;

export const settings = {} as const;

export const admin = {} as const;

export const composeErrors = {} as const;
