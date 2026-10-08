/**
 * Label name limits shared by the mail-side label dialog [E] and Settings → 标签 [F]: the
 * server rejects names longer than 64 code points (repo/accounts.cpp kMaxLabelName), so the
 * client counts code points too (not UTF-16 units: an emoji is one character).
 */
export const LABEL_NAME_MAX = 64;

/** Length in code points (what the server counts). */
export const labelNameLength = (name: string): number => Array.from(name).length;

/**
 * `maxLength` for a label name input: room above the limit (counted in UTF-16 units, so
 * surrogate pairs fit) so an over-long paste is reported instead of silently cut.
 */
export const LABEL_NAME_INPUT_MAX = LABEL_NAME_MAX * 2 + 20;
