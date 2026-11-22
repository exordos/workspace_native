# External integration settings (provider admin) — design

Status: draft (issue #633). Reference: workspace_ui `230e02b8b0273dee556a406746d122180a68b3e7`.
Server contract: exordos/workspace_backend `docs/en/workspace_api.md` (provider-admin rows, lines 347-351).

## Scope

Admin-only "Integration settings" entry in the account drawer's external-accounts
block plus the "External integration settings" dialog: overview, availability and
limits, custom CA readout, technical status (health), emergency suspend/resume.
Zulip is the only provider kind. Pure messenger surface; Electron shell: n/a.

## Server contract

Base: `/api/workspace/v1/messenger`.

| Method | Path | Notes |
| --- | --- | --- |
| GET  | `/external_provider_policies/zulip` | Policy + mandatory `ETag` header. 401 or 403 (`type == "ExternalResourceForbiddenError"`) => no access. |
| PUT  | `/external_provider_policies/zulip` | Header `If-Match: <etag>`. Body `{ "settings": { "kind": "zulip", "enabled", "limits", "custom_ca_bundle": null } }`. 412 (`type == "ExternalPreconditionFailedError"`) => conflict. |
| POST | `/external_provider_policies/zulip/actions/suspend/invoke` | Returns fresh policy DTO. |
| POST | `/external_provider_policies/zulip/actions/resume/invoke` | Returns fresh policy DTO. |
| GET  | `/external_provider_health/zulip` | Aggregate health DTO. |

DTOs (`manage-external-provider.types.ts`, `messenger-external-provider-admin.types.ts`):

- Policy: `uuid, created_at, updated_at, revision(>=1), provider=="zulip", enabled,
  emergency_suspended, limits{max_accounts<=100000, max_selected_chats_per_account<=1000000,
  max_file_bytes<=5368709120}, custom_ca_bundle` = null or
  `{uuid, generation>=1, sha256: 64-hex, certificate_count 1..32}`.
- Health: `provider, status in {healthy, unavailable}, account_counts, chat_counts,
  bridge_counts, operation_counts` (string->int maps),
  `metrics{queue_depth, selected_chats, synchronized_messages, synchronized_users}`,
  `updated_at`.

## Behaviour contract (hook.ts / lib.ts)

- Access probe = GET policy once when the entry mounts. Access denied on 401 or on
  403 with `ExternalResourceForbiddenError` => entry hidden entirely; a dialog open
  while access is lost closes itself.
- Draft = `{enabled, limits}` snapshot of the policy; Save enabled only when the form
  is valid, the draft differs from the loaded policy, no save is in flight and
  `custom_ca_bundle == null` (CA update is unsupported: the PUT body always sends
  `custom_ca_bundle: null`; a present bundle blocks Save with the ca.saveBlocked notice).
- Save: PUT with `If-Match: etag`; success replaces policy + etag and re-baselines the
  draft. 412 => conflict state: notice with "Load current settings" (resetDraft) and
  "Keep my draft"; after a conflict a background refresh stores the incoming policy and
  a three-way rebase (`rebaseExternalProviderPolicyDraft`) keeps per-field local edits
  (a field stays local iff it differs from the base; otherwise it takes incoming).
- A single mutation lock serializes save vs suspend/resume vs policy refresh; late
  replies from an aborted/stale request are dropped (request token discipline).
- Suspend requires the inline confirm panel (`role=alertdialog`); while an action runs
  the buttons show Suspending…/Resuming… and Save is blocked; on success the fresh
  policy replaces the loaded one (draft re-baselined only if it equaled the old base).
- Health loads with the dialog open; separate loading / loadError+Retry /
  refreshError states; counts render through status label maps (see i18n).

## UI structure and geometry (Tailwind -> px at 1x)

Entry (`manage-external-provider-entry.ui.tsx`, mounted after the external-accounts
list, `right-panel-user-menu.ui.tsx:311`): container `border-t border-border-subtle`
+ `pt-3` (top border + 12 px). Flat action button (no chevron): full width, row,
gap 10, rounded 8, 1 px border-border-subtle, background bg-elevated at 30 % alpha,
padding 10x8, hover bg-sidebar-hover; focus ring 2 accent. Leading badge 32x32,
rounded 6, background bg, icon `businessCenter` 18 px, text-secondary. Title 12 px
medium text-primary line-height 16; hint 11 px muted line-height 16, 2 px above.

Dialog (`manage-external-provider-dialog.ui.tsx`): AppDialog max width 768 px,
vertically centered, scrollable body; title + description; footer keeps a single
primary Save button (dismissing is the header X). Form sections (`rounded-xl border p-4`
= radius 12, padding 16, heading row `text-sm` 14 px semibold + optional description
12 px muted, content 16 px below):

1. Overview: provider row ("Zulip"), health pill, availability pill
   (policy.enabled => "Integration allowed"/"Integration disabled"), emergency row
   ("Integration suspended" / "Not active"), `Updated: <time>` + Refresh button.
   Pill: rounded-full, padding 8x4, 6 px dot + label; healthy => call-green on
   call-green 10 % alpha, otherwise danger.
2. Availability and limits: enabled switch (label 14 semibold + description 12 muted,
   switch accent-accent, role=switch), three numeric fields — max_accounts
   ("Accounts per user", max 100000), max_selected_chats_per_account
   ("Chats per external account", max 1000000), max file size shown as MiB
   (`bytes / 1048576`, field max 5120, suffix "MiB"). Field: label 12 medium
   secondary, input rounded 8 padding 12x8 text-sm on bg-elevated, inline error
   12 px danger "Enter a value from 0 to {maximum}." ; reduction hint 12 px muted.
3. Custom certificate authority: either "No custom certificate bundle is installed."
   (12 px muted) or a readout: certificates count, generation, SHA-256 fingerprint;
   when `custom_ca_bundle != null` a saveBlocked warning notice shows and Save stays disabled.
4. Technical status: header row (status pill + `Updated:` + Refresh), column grid
   `repeat(auto-fit, minmax(144px, 1fr))` gap 16 with Accounts / Chats / Bridge
   instances / Operations / Metrics blocks; each block heading 12 px semibold, rows
   are label(left) / tabular count(right) at 12 px, row gap 6. Empty => "No data".
   loadError / refreshError notices carry a Retry action.
5. Emergency controls (danger=true: border danger at 30 % alpha, background danger at
   5 % alpha, heading text-danger): description 12 px muted; when policy is active a
   "Suspend integration" danger button; pressing it swaps the section content to an
   inline confirm panel (title 14 semibold, description 12 muted, "Suspend for
   everyone" primary-danger + Cancel); when suspended => "Resume integration" button.
   Action failure notice: "Could not complete the action. The form settings were not
   changed."

Conflict notice (top of form): "Another administrator changed these settings. Your
draft is preserved in the form." + buttons "Load current settings" / "Keep my draft".

## i18n (87 keys, en + ru, `manageExternalProvider.*` + `externalAccounts.manageIntegrations(+Hint)`)

Native adds the same keys as `tr()` strings and fills `packages/native/translations/workspace_ru.ts`
(native-native-locale catalog guard). Key texts are the reference locales verbatim;
full table kept in the lane branch `tools/i18n/ext-provider-admin.json`. Placeholder
style `{{maximum}}`/`{{value}}` becomes `%1`. Notable: title "External integration
settings" / "Настройки внешних интеграций"; entry "Integration settings" /
"Настройки интеграций" with hint "Organization-wide external service settings" /
"Общие настройки внешних сервисов организации"; range error "Enter a value from 0 to
%1." / "Укажите значение от 0 до %1."; CA blocked notice; conflict buttons "Load
current settings"/"Keep my draft" and "Загрузить актуальные"/"Оставить мои правки";
suspend confirm "Suspend the Zulip integration?" / "Приостановить интеграцию Zulip?";
health status label maps for account/bridge/chat/operation/metric keys (44 short labels).

## Native counterpart

- New C++ model `NativeExternalProviderAdmin` (`packages/native/src/native-external-
  provider-admin.{h,cpp}`), pattern of `native-external-accounts.cpp`: ApiClient
  requests with request-token cancellation, `Q_PROPERTY` exposure of accessStatus,
  policyStatus, policy (QVariantMap), policyEtag, draft fields, healthStatus, health,
  saveStatus, actionStatus + error enums mirroring hook.ts; `Q_INVOKABLE` open/close/
  refreshPolicy/refreshHealth/save/suspend/resume/resetDraft/setEnabled/setLimit.
  Controller property `externalProviderAdmin`; probe starts when the external-accounts
  block expands while authenticated (entry mount parity).
- Entry UI appended in `packages/native/qml/NativeExternalAccountBlock.qml` after the
  connect button + list (`border-t` separator), objectNames
  `externalProviderEntry`, dialog `externalProviderDialog` (window-scoped), with
  objectNames for tests: `extProviderSave`, `extProviderLimitsField-{key}`,
  `extProviderRangeError`, `extProviderCaNotice`, `extProviderConflictNotice`,
  `extProviderSuspend`, `extProviderResume`, `extProviderHealthGrid`.
- Dialog reuses the existing native dialog shell pattern (header X + footer); width
  min(768, parent-32). Deviation note: QML Text has no `fontFeature`, so reference
  `tabular-nums` cannot be ported (documented elsewhere, same ruling).

## Acceptance

- native-controller: probe hides entry on 403 `ExternalResourceForbiddenError` and
  shows on 200; save sends `If-Match` and 412 `ExternalPreconditionFailedError`
  raises conflict; rebase keeps per-field local edits (three-way); limits beyond
  100000 / 1000000 / 5120 MiB mark the field invalid and disable Save; Save disabled
  while `custom_ca_bundle != null`; suspend/resume pending states and action error.
- native-window: entry appears after probe 200 fixture; dialog opens with the five
  sections at measured geometry; Save state follows dirty/valid/CA rules; suspend
  confirm panel replaces the action row. Window cases go through the standard
  bucket regeneration.
- Live: native-login on workspace.exordos.com; external-accounts block expand;
  entry visibility follows the live probe verdict (admin => open dialog, screenshot
  sections vs reference; non-admin => absent, note in PR).

No restyle of existing surfaces; pixel parity per NATIVE_DESKTOP_PARITY.md contract.
