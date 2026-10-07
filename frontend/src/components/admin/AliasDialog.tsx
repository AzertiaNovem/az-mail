/**
 * 创建 / 修改别名 dialog [WP-F]: address (local part + domain), display name, 共享已发送, and
 * the member list (multi-select of users, each with a 可代发 switch). The member list is sent
 * whole (PATCH replaces it wholesale).
 */
import { useMutation, useQueryClient } from '@tanstack/react-query';
import { useMemo, useState, type FormEvent } from 'react';
import { errorMessage, isApiError } from '@/api/client';
import { adminCreateAlias, adminKeys, adminUpdateAlias } from '@/api/admin';
import type { AdminAlias, AdminAliasInput, AdminUser, DomainRow } from '@/api/types';
import { Avatar, Button, Dialog, IconButton, Popover, Select, Switch, TextField } from '@/components/common';
import { t } from '@/i18n/zh';
import { toast } from '@/stores/toast';
import { ADMIN_NAME_MAX, hasFieldErrors, splitEmail, validateAlias, type AliasForm, type FieldErrors } from './validation';

export function aliasInput(f: AliasForm): AdminAliasInput {
  return {
    email: `${f.localPart.trim().toLowerCase()}@${f.domain}`,
    display_name: f.displayName.trim(),
    share_sent: f.shareSent,
    members: f.members.map((m) => ({ user_id: m.user_id, can_send_as: m.can_send_as })),
  };
}

function MemberPicker({ users, onPick }: { users: AdminUser[]; onPick: (u: AdminUser) => void }) {
  const [open, setOpen] = useState(false);
  const [q, setQ] = useState('');
  const needle = q.trim().toLowerCase();
  const shown = users.filter((u) => !needle || u.email.toLowerCase().includes(needle) || u.display_name.toLowerCase().includes(needle));
  return (
    <Popover
      open={open}
      onOpenChange={(o) => {
        setOpen(o);
        if (!o) setQ('');
      }}
      align="end"
      className="w-80 p-2"
      aria-label={t('admin.aliases.pickMembers')}
      trigger={
        <Button variant="text" size="sm" icon="person_add" disabled={users.length === 0}>
          {t('admin.aliases.pickMembers')}
        </Button>
      }
    >
      <TextField
        aria-label={t('admin.aliases.searchUsers')}
        placeholder={t('admin.aliases.searchUsers')}
        leadingIcon="search"
        value={q}
        autoFocus
        onChange={(e) => setQ(e.target.value)}
      />
      <ul className="m-0 mt-2 max-h-64 list-none overflow-y-auto p-0">
        {shown.length === 0 ? (
          <li className="px-3 py-4 text-center text-sm text-on-surface-variant">{t('admin.aliases.noUsers')}</li>
        ) : (
          shown.map((u) => (
            <li key={u.id}>
              <button
                type="button"
                className="flex w-full items-center gap-3 rounded px-3 py-2 text-left hover:bg-on-surface/8"
                onClick={() => onPick(u)}
              >
                <Avatar name={u.display_name} email={u.email} size={28} decorative />
                <span className="min-w-0 flex-1">
                  <span className="block truncate text-sm">{u.display_name || u.email}</span>
                  <span className="block truncate text-xs text-on-surface-variant">{u.email}</span>
                </span>
              </button>
            </li>
          ))
        )}
      </ul>
    </Popover>
  );
}

export interface AliasDialogProps {
  alias: AdminAlias | null;
  users: AdminUser[];
  domains: DomainRow[];
  onClose: () => void;
}

export function AliasDialog({ alias, users, domains, onClose }: AliasDialogProps) {
  const qc = useQueryClient();
  const [form, setForm] = useState<AliasForm>(() => {
    if (!alias) return { localPart: '', domain: domains[0]?.name ?? '', displayName: '', shareSent: true, members: [] };
    const [localPart, domain] = splitEmail(alias.email);
    return {
      localPart,
      domain,
      displayName: alias.display_name,
      shareSent: alias.share_sent,
      members: alias.members.map((m) => ({ user_id: m.user_id, can_send_as: m.can_send_as })),
    };
  });
  const [errors, setErrors] = useState<FieldErrors<AliasForm>>({});
  const [formError, setFormError] = useState<string | null>(null);

  const usersById = useMemo(() => new Map(users.map((u) => [u.id, u])), [users]);
  const memberInfo = (id: number) => {
    const u = usersById.get(id);
    const m = alias?.members.find((x) => x.user_id === id);
    return { email: u?.email ?? m?.email ?? `#${id}`, name: u?.display_name ?? m?.display_name ?? '' };
  };
  const candidates = users.filter((u) => !form.members.some((m) => m.user_id === u.id));
  const domainOptions = domains.map((d) => ({ value: d.name, label: d.name }));
  if (form.domain && !domainOptions.some((o) => o.value === form.domain)) domainOptions.unshift({ value: form.domain, label: form.domain });

  const mutation = useMutation({
    mutationFn: (input: AdminAliasInput) => (alias ? adminUpdateAlias(alias.id, input) : adminCreateAlias(input)),
    onSuccess: (a) => {
      void qc.invalidateQueries({ queryKey: adminKeys.all() });
      toast.push({ message: alias ? t('admin.aliases.updated', { email: a.email }) : t('admin.aliases.created', { email: a.email }) });
      onClose();
    },
    onError: (e) => {
      if (isApiError(e, 'address_exists')) setErrors({ localPart: errorMessage(e) });
      else if (isApiError(e, 'unknown_domain')) setErrors({ domain: errorMessage(e) });
      else setFormError(errorMessage(e));
    },
  });

  const set = <K extends keyof AliasForm>(k: K, v: AliasForm[K]) => {
    setForm((f) => ({ ...f, [k]: v }));
    setErrors((e) => ({ ...e, [k]: undefined }));
    setFormError(null);
  };

  const submit = (e?: FormEvent) => {
    e?.preventDefault();
    const errs = validateAlias(form);
    setErrors(errs);
    if (hasFieldErrors(errs)) return;
    mutation.mutate(aliasInput(form));
  };

  return (
    <Dialog
      open
      onOpenChange={(o) => !o && !mutation.isPending && onClose()}
      dismissible={!mutation.isPending}
      title={alias ? t('admin.aliases.editTitle', { email: alias.email }) : t('admin.aliases.createTitle')}
      size="lg"
      footer={
        <>
          <Button variant="text" onClick={onClose} disabled={mutation.isPending}>
            {t('actions.cancel')}
          </Button>
          <Button onClick={() => submit()} loading={mutation.isPending}>
            {alias ? t('actions.save') : t('actions.create')}
          </Button>
        </>
      }
    >
      <form className="flex flex-col gap-5" onSubmit={submit} noValidate>
        <div className="grid grid-cols-[1fr_auto_minmax(0,1fr)] items-start gap-2">
          <TextField
            label={t('admin.aliases.email')}
            placeholder="support"
            value={form.localPart}
            autoFocus={!alias}
            autoCapitalize="off"
            spellCheck={false}
            error={errors.localPart}
            onChange={(e) => set('localPart', e.target.value)}
          />
          <span className="pt-8 text-on-surface-variant">@</span>
          <Select
            label={t('admin.users.domain')}
            options={domainOptions}
            value={form.domain}
            error={errors.domain}
            onValueChange={(v) => set('domain', v)}
          />
        </div>
        <TextField
          label={t('admin.aliases.displayName')}
          value={form.displayName}
          maxLength={ADMIN_NAME_MAX + 20}
          error={errors.displayName}
          onChange={(e) => set('displayName', e.target.value)}
        />
        <div className="flex flex-col gap-1">
          <Switch checked={form.shareSent} onCheckedChange={(v) => set('shareSent', v)} label={t('admin.aliases.shareSent')} />
          <p className="m-0 pl-[64px] text-xs text-on-surface-variant">{t('admin.aliases.shareSentHelp')}</p>
        </div>

        <section aria-label={t('admin.aliases.members')} className="flex flex-col gap-2">
          <div className="flex items-center gap-2">
            <h3 className="m-0 flex-1 text-sm font-medium text-on-surface">
              {t('admin.aliases.members')}
              <span className="ml-2 font-normal text-on-surface-variant">{t('admin.aliases.memberCount', { count: form.members.length })}</span>
            </h3>
            <MemberPicker users={candidates} onPick={(u) => set('members', [...form.members, { user_id: u.id, can_send_as: true }])} />
          </div>
          {form.members.length === 0 ? (
            <div className="rounded-lg border border-dashed border-outline-variant px-4 py-5 text-center text-sm text-on-surface-variant">
              <p className="m-0">{t('admin.aliases.noMembers')}</p>
              <p className="m-0 mt-1 text-xs">{t('admin.aliases.noMembersHint')}</p>
            </div>
          ) : (
            <ul className="m-0 flex list-none flex-col rounded-lg border border-divider p-0">
              {form.members.map((m) => {
                const info = memberInfo(m.user_id);
                return (
                  <li key={m.user_id} className="flex items-center gap-3 border-b border-divider px-3 py-2 last:border-b-0">
                    <Avatar name={info.name} email={info.email} size={28} decorative />
                    <span className="min-w-0 flex-1">
                      <span className="block truncate text-sm">{info.name || info.email}</span>
                      {info.name && <span className="block truncate text-xs text-on-surface-variant">{info.email}</span>}
                    </span>
                    <Switch
                      checked={m.can_send_as}
                      aria-label={`${t('admin.aliases.canSendAs')} ${info.email}`}
                      title={t('admin.aliases.canSendAsHelp')}
                      onCheckedChange={(v) =>
                        set(
                          'members',
                          form.members.map((x) => (x.user_id === m.user_id ? { ...x, can_send_as: v } : x)),
                        )
                      }
                      label={<span className="text-xs text-on-surface-variant">{t('admin.aliases.canSendAs')}</span>}
                    />
                    <IconButton
                      icon="close"
                      size="sm"
                      label={t('admin.aliases.removeMember', { name: info.name || info.email })}
                      onClick={() =>
                        set(
                          'members',
                          form.members.filter((x) => x.user_id !== m.user_id),
                        )
                      }
                    />
                  </li>
                );
              })}
            </ul>
          )}
        </section>
        {formError && (
          <p role="alert" className="m-0 rounded-lg bg-error-container px-3 py-2 text-sm text-on-error-container">
            {formError}
          </p>
        )}
        <button type="submit" hidden aria-hidden="true" tabIndex={-1} />
      </form>
    </Dialog>
  );
}
