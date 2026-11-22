# Topic summary settings dialog (issue #635)

Design oracle: `exordos/workspace_ui` master `e0194ffb667986b58a12256ce41ca5dc99b56b83`
(feature dir `packages/web/src/features/topic-summary-settings/`, contract issue cites
`75bd21f37de94728e2ebe100acfea24ac9c1a4e1`; re-measured at the newer sha above).
Figma token unavailable in this run - mockups not consulted; reference code is the oracle.

## Purpose

Admin surface for AI topic summaries opened from the gear button on the Topic context
summary panel (shipped in #133, `qml/NativeTopicSummary.qml`, objectName
`channelTopicSummarySettings`). Today the native gear opens a placeholder popup with a
single enable switch; this release replaces it with the reference three-tab dialog:

1. **This topic** - per-topic summary generation switch, custom system prompt, reasoning effort.
2. **Common settings** (gates) - project-level `global_enabled` x `project_enabled` switches.
3. **LLM endpoints** - global OpenAI-compatible endpoint CRUD.

Tabs are permission-gated; a user sees only the tabs they may edit.

## Permissions (`entities/messenger/messenger-topic-summary-permissions.lib.ts`)

- `topic` tab: allowed iff the stream snapshot belongs to the current user and the
  user's stream role is `owner` or `administrator`; `unknown` when the snapshot is
  missing/foreign; else `denied`.
- `gates` tab: `allowed` iff capabilities contain `workspace.topic_summary_settings.manage`.
- `endpoints` tab: `allowed` iff capabilities contain `workspace.topic_summary_endpoint.manage`.
- Gear visible iff any tab is `allowed`. With zero allowed sections the dialog does not open.
- Native: capability list and stream role must come from the session/channel DTOs already
  parsed by `src/session.cpp`; extend them if the fields are not exposed yet.

## API contract (`shared/api/messenger-topic-summary-management.api.ts`)

- `GET  /topic_summary_settings/{projectUuid}` -> `{project_id, global_enabled, project_enabled}`
- `PUT  /topic_summary_settings/{projectUuid}` body `{global_enabled, project_enabled}`
- `GET  /topic_summary_endpoints/` -> list; `GET /topic_summary_endpoints/{uuid}` -> item.
  Response fields `claim_expires_at, last_success_at, last_failure_at, last_error_code`
  are defaulted to null when absent (normalize before parsing).
- `POST /topic_summary_endpoints/` create; uuid client-generated; `api_key` required on create.
- `PUT  /topic_summary_endpoints/{uuid}` partial update (at least one mutable field);
  `api_key` omitted when unchanged (write-only credential, echoed back only as
  `credential_present: bool`).
- `DELETE /topic_summary_endpoints/{uuid}` -> no body.

Endpoint DTO fields: `uuid name base_url model enabled priority supports_vision
supports_reasoning temperature max_output_tokens top_p presence_penalty frequency_penalty
credential_present claim_expires_at last_success_at last_failure_at failure_count
last_error_code created_at updated_at`.

Topic-level config rides the existing topic summary PATCH surface used by #133
(`summary_enabled`, `summary_system_prompt`, `summary_reasoning_effort`), patch = only
dirty fields, `null` prompt = app default, reasoning: `null|off|minimal|low|medium|high`.

## Behavioural rules (`topic-summary-settings.lib.ts`, `topic-summary-endpoints.lib.ts`)

- Prompt: trimmed before save; custom prompt non-empty and <= 16384 chars
  (`TOPIC_SUMMARY_CUSTOM_PROMPT_MAX_LENGTH`); empty/whitespace -> `promptEmpty` error.
- Gates effective formula: `global_enabled AND project_enabled` (banner switches to
  green "Generation is active" vs neutral "Generation is paused").
- Drafts are three-way rebased (base/local/incoming) so a live server update does not
  clobber unsaved local edits; the network error keeps the draft for retry
  (`status.network` text says so).
- Operation errors map: 400 `invalid`, 401/403 `forbidden`, >=500 `server`,
  contract (schema mismatch) `contract`, else `unknown`; denied permission shows the
  `status.forbidden` banner and disables controls.
- Endpoint validation: name/model required and <=255; base URL valid http(s) URL,
  no userinfo/query/hash, <=2048 chars, trailing slashes trimmed; api_key required on
  create, <=8192; priority int 0..1000000; max_output_tokens int 1..32768; temperature
  0..2; top_p 0..1; presence/frequency penalty -2..2. Create defaults: enabled=true,
  priority=100, vision/reasoning=false, temperature=0.2, max_output_tokens=512,
  top_p=1, penalties 0.
- Delete asks confirmation naming the endpoint (`endpoints.deleteConfirm`).

## UI geometry (reference measured, rem at 16px)

- Dialog shell: `w-[calc(100%-2rem)] max-w-[70rem]` (1120 px cap), height per tab:
  topic 680 px, gates 600 px, endpoints 760 px (each `min(Xrem,92vh)`); vertically centered.
- Header 88 px (`px-32 pt-24`): title text-xl semibold + subtitle `# topic name`
  (+ ` . channel` suffix) text-[13px] muted; close icon button right.
- Tab strip 52 px, bottom border `border-border-subtle`, tab padding px-16 py-12,
  active tab 2px bottom accent underline; keyboard: ArrowLeft/Right wrap, Home/End,
  roving tabIndex; a height change follows the active tab.
- Section body scrolls; section footer 80 px (`h-20`) with unsaved-yellow-dot +
  `status.unsaved`, green `status.saved` on success, Cancel (min-w 112 px) + Save
  (min-w 144 px) disabled unless dirty/pending/denied rules hold.
- Topic tab: enable SwitchRow, system prompt textarea rows=6 min-h 144 px resize-y,
  "Use default prompt" button, reasoning effort selector (Provider default + 5 levels).
- Switch row: track 36x20 rounded-full (`border-subtle` -> `accent`), knob 16x16 at
  2 px inset, translate 16 px when on.
- Inputs: rounded-lg border `border-border-subtle` bg `text-field-bg` px-12 py-8 text-sm;
  focus accent border + 2px `accent/15` ring; disabled opacity 60.
- Error notice: rounded-lg border `danger/30` bg `danger/5` text `danger` px-12 py-10.
- Palette token names used: accent, text-primary, text-muted, text-field-bg,
  border-subtle, bg-elevated, danger, indicator-yellow, call-green.

## i18n

Every visible string is a `topicSummarySettings.*` key with ru + en values in
`packages/web/src/i18n/locales/{ru,en}.json` (extracted in the issue; ~95 keys).
Native must mirror every new `tr()` string into
`packages/native/translations/workspace_ru.ts` (locale guard test).

## Native implementation plan

1. Controller (`src/native-controller.*`, API in `src/api_client.*`): settings get/put,
   endpoints CRUD with the same DTO validation, error mapping and rebase semantics;
   expose `topicSummary*` Q_PROPERTYs for the dialog. Permission resolution helper on top
   of session capabilities + channel role.
2. QML: `qml/NativeTopicSummarySettingsDialog.qml` (AppDialog-equivalent shell, 3 tabs)
   replacing the placeholder popup in `NativeTopicSummary.qml`; keep the gear objectName
   and Accessible.name so existing #133 tests keep their entry handle.
3. Tests: controller group (mock server): settings roundtrip + dirty-field gating, gates
   formula, endpoint CRUD incl. write-only key semantics (api_key omitted on update when
   unchanged) and every validation rejection; window group: tab visibility by permission,
   ArrowLeft/Right traversal, footer gating, denied banner. New cases get bucket
   fragments via `scripts/gen-test-buckets.py` (#675 recipe).

## Deviations

- The #133 placeholder popup is replaced (new functionality extends the shipped entry;
  the entry button itself keeps its objectName and position).
