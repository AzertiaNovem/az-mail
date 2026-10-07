/**
 * Admin tab page [WP-F]. PLACEHOLDER written by WP0 so the router (WP-E) builds; WP-F replaces it.
 * Contract (kept by the replacement): `export function AdminOutbox()`, no props, rendered inside
 * AdminLayout's `<Outlet/>`. Data comes from api/admin.ts (`adminKeys` + admin* calls).
 */
export function AdminOutbox() {
  return <p className="text-on-surface-variant">发件队列</p>;
}
