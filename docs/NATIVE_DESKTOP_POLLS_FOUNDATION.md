# Polls foundation: payload contract, REST actions, store and realtime merge

Issue #214 (parent Epic #213). This is the data layer only: message parsing, the
wire DTO guards, REST action clients, cache persistence and the realtime merge
rule. No poll UI ships here; issues #215/#216 build presentation on top of the
snapshot this document defines. The contract of record is the reference app at
`workspace_ui 230e02b8b0273dee556a406746d122180a68b3e7`:
`docs/plans/message-polls.md:209-441`, `packages/web/src/shared/api/messenger.types.ts`,
`packages/web/src/shared/api/messenger-messages.api.ts`,
`packages/web/src/entities/messenger/messenger-adapters.lib.ts`,
`packages/web/src/entities/messenger/messenger-poll-actions.lib.ts` and
`packages/web/src/entities/message/message.model.ts`. This is parity work: the
native client mirrors that contract and invents nothing.

## Wire contract

A poll message is an ordinary messenger message whose `payload.kind` is `poll`
and which carries a sibling `poll_state` object on the DTO. Payload fields:

| Field | Wire name | Notes |
| --- | --- | --- |
| questions | `questions[]` | contract limits on creation: 1..20 questions, 2..20 options each, question text <= 1000 chars, option text <= 200 chars |
| question | `{uuid, text, options[{uuid,text}], correct_option_uuids?}` | client-generated UUIDs on creation |
| multiple answers | `multiple_answers` | required bool, global for the poll |
| anonymity | `is_anonymous` | bool |
| revote | `allow_revote` | bool |
| results visibility | `results_visibility` | `"always"` or `"after_vote"` |
| member options | `allow_adding_options` | bool, default `false` |
| deadline | `closes_at` | datetime-or-null; null means no automatic close |
| closed at | `closed_at` | read-only datetime-or-null |

Poll state `poll_state`: `{revision, is_closed, voter_count, questions[].{
question_uuid, voter_count, option_counts, option_voter_previews?}, my_votes[].{
question_uuid, option_uuids}}`. `revision` is the server-owned poll-state counter
that never moves the message content revision; voting changes `poll_state` only.

## DTO guards (mirror of `messenger.types.ts`)

`parseMessage` in `src/domain.cpp` enforces exactly the reference guards:

- Poll payload guard: `questions` is an array of `{uuid: UUID, text: string,
  options: array of {uuid: UUID, text: string}, correct_option_uuids?: array of
  UUID}`; `multiple_answers`, `is_anonymous`, `allow_revote` are required typed
  booleans; `results_visibility` is exactly `"always"` or `"after_vote"`;
  `allow_adding_options` is absent or a boolean; `closes_at`/`closed_at` are
  datetime-or-null (the reference `isDateTime` is a non-empty trimmed string).
- Poll state guard: `revision` is an integer > 0; `is_closed` is boolean;
  `voter_count` is a non-negative integer; every question has `question_uuid`
  UUID, non-negative `voter_count`, `option_counts` as UUID -> non-negative
  integer, and optional `option_voter_previews` whose entries are UUID-keyed
  non-empty, duplicate-free UUID arrays; `my_votes` entries are
  `{question_uuid: UUID, option_uuids: UUID[]}`.
- `isWorkspaceMessengerMessageDto` rule: a poll message REQUIRES a valid
  `poll_state`; every other kind must have `poll_state` absent or null. A row
  failing its guard parses as `std::nullopt`.

History pages keep the existing native all-or-nothing rule: one unparseable row
fails the page with the existing "invalid conversation history" report and the
previously committed rows stay visible. That is the native shape of the
reference's strict list parsing (`parseStrictDtoList` throws instead of dropping
the item): a malformed poll never silently drops rows from the page, it keeps
the page intact as a unit. Valid polls parse end to end, so a mixed history that
contains polls no longer loses them.

## Model

`workspace::Message` grows two typed snapshots, filled only for kind `poll`:

- `PollPayload poll`: questions (uuid/text/options/correctOptionUuids),
  multipleAnswers, isAnonymous, allowRevote, resultsVisibility,
  allowAddingOptions (default false), closesAt/closedAt (empty string means
  null).
- `PollState pollState`: revision, isClosed, voterCount, per-question
  `{questionUuid, voterCount, optionCounts, optionVoterPreviews(+presence flag)}`
  and myVotes.

`NativeController::messages()` exposes the snapshot for poll rows as an extra
row key `"poll"` (camelCase QVariantMap mirroring the domain fields, with the
state under `"state"`). Row presence itself is the end-to-end parse proof; #215
renders it. No new `Q_PROPERTY` is introduced, so no new NOTIFY obligations.

## Realtime and mutation merge (mirror of `message.model.ts`)

`applyPollRevisionGuard(incoming, previous)` ports the store rule: when both
rows are polls with state, a lower incoming revision never overwrites - the
incoming row keeps the previous payload and state; at equal revision an existing
`is_closed: true` is never reverted to false (the flag is forced back on). The
guard runs in the realtime `message.updated` path and in every poll action
response before the row is replaced, so voting never changes content revision,
ordering, unread counters or notifications: the merged row keeps its
`createdMicros` (ordering), its read flag (monotonic) and never enters the
notification-candidate path. When the guard fully rejects an incoming snapshot,
the cache projection for that message omits `Projection::Payload` so the stored
snapshot cannot regress either.

## Cache persistence

`NativeCache::sanitize` keeps the full poll payload whitelist (`questions`,
`multiple_answers`, `is_anonymous`, `allow_revote`, `results_visibility`,
`allow_adding_options`, `closes_at`, `closed_at`) and the row-level
`poll_state` beside the payload. Type validity is already proven by the strict
parse inside the same sanitize pass, so poll rows do not pass through the
string-field shape check that fits the string-only payloads. Read-back
re-sanitizes, so a tampered cache row is dropped like every other kind.

## REST actions (mirror of `messenger-messages.api.ts`)

All under `/api/workspace/v1/messenger`, bearer-authenticated through the
existing `ApiClient`:

- `POST /messages/` create: the client generates every question and option UUID
  (client-generated uuids are part of creation). Send guard mirrors
  `supportsMessengerPolls` (`messenger-poll-actions.lib.ts:18-26`): allowed
  only when the stream source is `native` or `zulip`, encryption is not true
  and the stream is not archived. The server answers 400 for E2EE/Zulip
  violations and 409 "Poll creation is not enabled" when the flag is off; the
  client surfaces the failure without mutating local state.
- `POST /messages/{uuid}/actions/vote/invoke` `{question_uuid, option_uuid,
  selected}`: `selected` is the desired state, not a toggle - a repeat of the
  current state succeeds without flipping it.
- `POST /messages/{uuid}/actions/close_poll/invoke` `{}`; idempotent, no reopen.
- `POST /messages/{uuid}/actions/update_settings/invoke` `{settings:
  {closes_at, allow_adding_options}}`: both fields always present - this is the
  full editable configuration save, not a patch.
- `POST /messages/{uuid}/actions/add_poll_option/invoke` `{question_uuid,
  option: {uuid, text}}`: the client generates the option UUID once and reuses
  it on retry; an exact uuid+text replay answers 200 with the unchanged message
  (even closed or at the 20-option limit) and the client applies it without
  error or duplicated option; same uuid with other text is a server 409.
- `GET /messages/{uuid}/poll_voters/?question_uuid&option_uuid&page_limit&
  page_marker`: default page 50, hard client cap 100 (server max 100); UUID-
  ordered positional marker; the page arrives with the strict `{uuid}` item
  guard and `X-Pagination-*` markers via the existing `ApiResult.nextMarker`.

Action responses are parsed as message DTOs, verified to be the same poll
message, merged through the revision guard, projected to the cache and published
through the existing `messagesChanged` signal. Failures never mutate the model;
`pollActionFailed(messageUuid, action, detail)` carries a technical detail code
(`http-4xx`, `invalid-response`, `stale-response`) so the UI tickets own all
user-visible strings. No new `tr()` strings ship in this ticket.

## Test surface

- `domain-test`: full-field poll parse; the guard matrix above (including the
  non-poll `poll_state` rule and preview uniqueness); `applyPollRevisionGuard`.
- `native-cache-test`: a history page containing a poll round-trips payload and
  `poll_state` through put/get.
- `native-controller-test`: poll row end-to-end from history into the model with
  all config fields; a page containing a malformed poll is rejected as a whole
  (no partial application, so good and bad rows never mix);
  revision-guard merge over `applyRealtimeEvent` including the `is_closed`
  ratchet; vote invoke posts the desired-state body and merges without touching
  order/read state; add_poll_option replay accepted at 200 without duplication;
  create guard and body; update_settings always carries both fields; poll
  voters strict page and marker.
