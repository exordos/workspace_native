# Native desktop call launch: meeting URL contract, meet host, single active call

Status: implemented for #161. Part of the #native-call series on top of the
embed-stack decision in [NATIVE_DESKTOP_CALL_STACK.md](NATIVE_DESKTOP_CALL_STACK.md)
(QtWebEngine hosting the Jitsi Meet IFrame External API, issue #160). This
document covers the launch/join contract only: room-name and meeting-URL
construction, per-workspace meet-host resolution, the single-active-call store,
the meeting-URL message and the message-list call card. The WebEngine call
window itself is a follow-up issue; here "open" means the store transitions to
an active call and the launch is observed by the UI.

## Reference contract

All reference line numbers are from `workspace_ui` at
`61dacef78dab2f5d0be76f283bebf49fe29b84af` (read-only reference clone).

- Room name (`packages/web/src/features/jitsi-call/workspace-jitsi-call.lib.ts:27-45`):
  six segments joined with `-`: `workspace`, organization id, project id,
  stream UUID, topic UUID, `nowMs`. Each segment is trimmed, every run of
  non-`[a-z0-9-]` characters collapses to `-`, leading/trailing `-` are
  stripped, the segment is lowercased, and an empty segment becomes `room`.
  The meeting URL is `${meetOrigin}/${encodeURIComponent(roomName)}`.
- Call identity (`packages/web/src/features/jitsi-call/jitsi-call.model.ts:56-71`):
  `callKey = ownerKey + ":" + normalizedMeetingUrl`, where normalization trims
  whitespace and trailing slashes, lowercases scheme and host, and re-trims
  trailing slashes; an unparseable URL falls back to the trimmed string.
  `startWithVideoMuted` defaults to `true` (`:74`). `requestOpenCall` returns
  `opened`, `same` (same callKey as the active call; the original active call
  object is preserved unchanged) or `blocked-active` (different callKey; the
  active call is preserved). Accepting an incoming invite while a call is
  active clears the invite without replacing the active call.
- Meet host (`packages/web/src/features/jitsi-call/jitsi-call-settings.model.ts:11-24`,
  `packages/web/src/widgets/layout/layout-workspace-messenger-bootstrap.hook.ts:148`):
  the Workspace `server_settings.meet_url` value is reduced to its origin
  (http/https only, anything else rejected), kept per account owner in memory
  and cleared on logout. It overrides the build-time `VITE_JITSI_MEET_DOMAIN`.
  Meeting URLs are accepted on the configured host or on `meet.jit.si`
  (`packages/web/src/shared/lib/jitsi.ts:30-65,77-106`).
- Start flow (`packages/web/src/pages/chat/chat-page-workspace.ui.tsx:2148-2260`):
  the header action first posts the meeting URL as a plain markdown message
  into the target stream/topic; the call is opened only after the send is
  applied. A failed or unconfirmed send shows the inline send error and opens
  nothing. If a different call is already active the action is refused with the
  generic inline error before anything is sent. While a call is active, opening
  the same call key again is a no-op.
- Header entry point (`packages/web/src/pages/chat/chat-page-workspace.ui.tsx:4100`):
  the call button is rendered only when the Workspace meet host is configured
  (`server_settings.meet_url`, origin-only; the build-time default domain does
  not enable the button).
- Call card (`packages/web/src/widgets/workspace-message-list/workspace-message-bubble-jitsi-card.ui.tsx`):
  a message body containing a meeting URL renders as a card instead of plain
  text: the label `call.callMessage` ("Call") in call-green `#26c038`, the room
  display name (room with `-`/`_` runs replaced by spaces; fallback
  `call.callName` "Call name"), an accent divider followed by the location
  name when present, up to three participant-initial avatars with a `+N`
  overflow chip, click/Enter/Space activation and the accessible name
  `call.joinCall` ("Join call"); 15px text.
- Locale keys (`packages/web/src/i18n/locales/{en,ru}.json`): `call.call`
  "Call"/"Позвонить", `call.callMessage` "Call"/"Звонок", `call.callName`
  "Call name"/"Название звонка", `call.joinCall` "Join call"/"Подключиться".

## Native module

New module `packages/native/src/call` (compiled into `workspace-native-core`):

- `call/jitsi-call.h/.cpp` — the pure URL model, a direct port of the
  reference helpers: `normalizeRoomSegment`,
  `buildWorkspaceJitsiRoomName(org, project, stream, topic, nowMs)`,
  `normalizeMeetOrigin` (origin-only, http/https only, trailing slashes
  trimmed), `effectiveMeetBase` (server-settings meet_url first, then the
  build-time default `https://meet.jit.si`, the native counterpart of
  `VITE_JITSI_MEET_DOMAIN`), `buildJitsiMeetingUrl`, `parseJitsiUrl`,
  `getJitsiMeetingUrl` (configured host or `meet.jit.si` only),
  `createJitsiCallKey` and `formatRoomDisplayName`.
- `call/native-call-store.h/.cpp` — `JitsiCallStore`, the single-active-call
  store with the reference state machine: `requestOpenCall` →
  `opened`/`same`/`blocked-active`, `closeCall`, `ingestIncomingInvite`,
  `acceptIncomingInvite` (never replaces an active call; always clears the
  invite), `declineIncomingInvite`, `clear`. Active call fields: `callKey`,
  `meetingUrl`, `locationName`, `ownerKey`, `meetUrl`, `displayName`,
  `startWithVideoMuted` (default `true`), `startedAtMs`.

`NativeController` owns one `JitsiCallStore` (exposed as the QML-visible `calls`
property) and resolves the per-workspace meet host: on each authenticated
project session it fetches `/api/workspace/v1/messenger/server_settings`,
normalizes `meet_url` to its origin and keeps it in memory (`meetHost`,
empty when unset or invalid); it is cleared when the session signs out, like
the reference per-owner store. `canStartCall` is the header-button gate
(authenticated conversation + configured `meetHost`), mirroring
`chat-page-workspace.ui.tsx:4100`. `startCall()` mirrors the reference start
flow: build the room from organization host, project, selected stream/topic and
`nowMs`; refuse with the inline `callError` when another call is active or the
meet host is missing; POST the meeting URL as a markdown message to the target
conversation; open the call only after the server confirms the message.

QML: the message bubble renders `NativeJitsiCard` instead of the text body when
the message body contains a meeting URL on the effective host, and the chat
header shows the call button only while `canStartCall` holds. The card uses the
reference locale strings listed above; every new `tr()` source string ships
with its reviewed Russian catalog entry in
`packages/native/translations/workspace_ru.ts`.

## Deviations from the reference (recorded)

- The call window itself (QtWebEngine embed) is the follow-up issue; here an
  opened call only updates the store and header/bubble presentation. The
  embed-stack doc defines how the window attaches.
- Participant chips: the reference polls participant lists per URL
  (`call.model` participants store, 5s poll). No native participant polling
  exists yet, so the card renders the message author as the single initial
  avatar — exactly the reference fallback path when no participants are known.
- Encrypted (E2EE) streams: the reference sends the meeting URL through the
  encrypted chat session. Until the native encrypted-send entry point accepts
  a programmatic body, `startCall` on an encrypted stream reports the inline
  error and sends nothing (no plaintext leak); plain streams implement the full
  contract.
- Build-time default host: `VITE_JITSI_MEET_DOMAIN` becomes the compiled
  default `https://meet.jit.si`, used only for URL recognition when the
  Workspace setting is absent — it never enables the header button, matching
  the reference.

## Tests

CTest group `native-native-call` (`packages/native/tests/native-call-test.cpp`):
deterministic room names with fixed `nowMs` and the reference normalization
matrix; meeting-URL building; `callKey` opened/same/blocked-active matrix
mirroring `jitsi-call.model.test.ts` (including same-call field preservation and
accept-during-active); meet_url origin normalization (non-http(s) rejected,
trailing slashes trimmed); URL acceptance limited to the configured host and
`meet.jit.si`. `native-native-controller` gains the wiring cases: meet host
from `server_settings.meet_url` (origin-only) and its absence clearing the
header gate, the start flow posting the meeting-URL markdown message before
opening, refusing while another call is active, and nothing opening when the
send is not confirmed. Window cases cover the header button presence/absence
and the bubble card rendering.
