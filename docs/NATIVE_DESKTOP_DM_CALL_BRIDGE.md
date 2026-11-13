# Native desktop DM call initiation bridge (#164)

Status: implemented. Part of #33. Builds on the merged call stack docs:
[NATIVE_DESKTOP_CALL_STACK.md](NATIVE_DESKTOP_CALL_STACK.md) (#160 embed decision),
[NATIVE_DESKTOP_CALL_LAUNCH.md](NATIVE_DESKTOP_CALL_LAUNCH.md) (#161 `JitsiCallStore`,
meet host, header start flow) and
[NATIVE_DESKTOP_INCOMING_CALL_UI.md](NATIVE_DESKTOP_INCOMING_CALL_UI.md) (#163).
This issue adds the *bridge*: starting a DM call from another user's profile panel
and from the chat info drawer, with DM auto-create, pending handoff, one shared
in-flight guard, stale-completion drop and E2EE refusal.

## Reference contract

All reference line numbers are from `workspace_ui` at
`61dacef78dab2f5d0be76f283bebf49fe29b84af` (read-only clone), re-measured from that sha.

### Bridge store — `packages/web/src/features/chat-dm-call-bridge/chat-dm-call-bridge.model.ts:29-59`

- `pendingDmCallPartnerUserUuid: string | null` with `setPendingDmCallPartnerUserUuid(uuid)`
  (plain set) and `clearPendingDmCallPartner()` — a **no-op when already empty** (early
  `return` at `:50`, no store write, so no re-render), plus `logStoreAction` on both writes.
- `activeChatCall: { ownerKey, runtimeGeneration, streamUuid, invoke } | null` with
  `setActiveChatCall(call|null)` (`:30-31`).
- `setInvokeDmCallFromProfileHandler` / `invokeDmCallFromProfile` (`:48-59`) has no
  production consumer at the sha; skipped in the port (recorded below).
- Store test contract: `chat-dm-call-bridge.model.test.ts:10-35` (set/clear roundtrip,
  handler invoke, missing-handler no-op).

### Profile panel Call — `packages/web/src/widgets/right-panel/right-panel-user-profile.ui.tsx:472-536`

`handleCall` (guards: `messagePending || callPending || runtimeContext == null ||
isOwnProfile`):
1. `resolveDirectProfileChatNavigation` for the partner;
   `missing-default-topic` -> log error, return **before** setting pending (`:481-489`).
2. `setPendingDmCallPartnerUserUuid(profile.userUuid)` (`:491`).
3. `already-open` -> stay (the ChatPage effect picks the pending up on the spot).
4. `open-default-topic` -> navigate to the default topic.
5. stream missing: `setCallPending(true)`, `runWorkspaceDirectStreamCreate(...)`;
   not-applied or throw -> `clearPendingDmCallPartner()` (`:507`, `:513`) + log;
   applied -> navigate to the new default topic; `finally setCallPending(false)`.
`callPending` disables the Call chip while the DM is created.

### Action chip — `packages/web/src/widgets/right-panel/right-panel-user-profile-actions.ui.tsx`

- Other-user row (`:103-127`): Message | Call | Share, three equal fill buttons,
  `flex gap-2` (8 px), each `min-w-0 flex-1`.
- Call action (`:113-120`): label `call.call`, icon `phone`, `iconSize={30}`
  ("phone glyph ~31x31 in padded viewBox — slightly under the 40 box"),
  `disabled={callPending}`, testId `right-panel-profile-call`.
- Chip chrome (`:11-40`, Figma 12709:38520): `rounded-lg` (8 px radius) `bg-card-bg`
  `px-2 py-2` (8 px padding), centered column, `gap-0`; icon hit box `h-10 w-10`
  (40x40) with the glyph tinted `text-icon-active`; label `text-xs leading-4`
  (12 px / 16 px line) `text-text-secondary`, `truncate` for fill buttons;
  `disabled:opacity-50`; hover swaps the surface to `bg-bg`.
- This is exactly the chip #134 already ships in `qml/NativeUserProfile.qml`
  (`ProfileChip`, objectNames `profileAction-<id>`, `profileActionGlyph-<id>`,
  `profileActionLabel-<id>`); #164 activates the `call` chip, keeping geometry.

### Pending consumer + registration + start — `packages/web/src/pages/chat/chat-page-workspace.ui.tsx`

- Reactive subscription `:863-865` (`useChatDmCallBridgeStore(s => s.pendingDmCallPartnerUserUuid)`)
  so the handoff fires even when the DM is already open (pending changes without a
  route change).
- Pending consumer `:2300-2316`: skip favorites / non-`directPrivate` / missing
  `workspaceMeetUrl`; when `pendingDmCallPartnerUserUuid === headerView.directUserUuid`
  -> `clearPendingDmCallPartner()` then start the header call — exactly once, because
  the clear makes every re-entry a no-op while the same DM stays open.
- Active-call registration `:2264-2297` (`useLayoutEffect`): skipped when there is no
  runtime/owner/meet url, for favorites, or when `stream.encryption && !encryptedChatAvailable`;
  otherwise registers `{ ownerKey, runtimeGeneration: runtimeContext.runtimeGeneration,
  streamUuid, invoke }`; the invoke is guarded by `captureWorkspaceChatUiAction()` (stale
  route/unmount -> no-op); the effect cleanup clears the store entry only if it is
  still the same object (self-clearing on unmount).
- `handleStartWorkspaceHeaderCall` `:2148-2255`: clear inline error; runtime missing ->
  `workspaceMessenger.runtimeUnavailable`; meet url missing -> `message.sendFailed`;
  blocked send target -> its error; `pendingWorkspaceJitsiHeaderCallRef.current` (`:641`)
  in-flight guard -> plain return (header and sidebar share this one ref); E2EE stream
  without `cryptoSession`/`sendKeyUuid` -> `e2ee.cryptoUnavailable`, nothing sent
  (test `:5328-5341`); build meeting url (`features/jitsi-call/workspace-jitsi-call.lib.ts:41-45`
  from per-workspace `server_settings.meet_url`, `:859-861`); a different active call
  -> `message.sendFailed` before sending; set the ref; send via the E2EE session
  (`cryptoSession.send`) or `sendMessengerMessage` wrapped in the runtime
  `AbortSignal`; on completion: stale `!isUiActionCurrent()` -> drop (no open, no error,
  and the `finally` does not reset the guard — `:2245`); non-applied -> `message.sendFailed`;
  owner changed -> drop; then `requestOpenCall` (`blocked-active` -> `message.sendFailed`).
  Test matrix `:5277-5325` (header+sidebar share the guard: double invoke -> one encrypted
  send whose text contains the meet url; call opens with exactly the sent url only after
  the send resolves; unmount clears the descriptor and a later invoke no-ops) and
  `:5597-5665` (old completion after A->B->A navigation opens nothing and does not reset
  the newer guard; a runtime-generation change aborts the in-flight invite via AbortSignal).
- Sidebar call surface — `packages/web/src/widgets/right-panel/right-panel-workspace-info.ui.tsx:841-849`
  (selector: `onCall = call.invoke` only when `call.ownerKey === ownerKey &&
  call.runtimeGeneration === runtimeGeneration && call.streamUuid === streamUuid`,
  else undefined), wired at `:866`. Behaviour test
  `right-panel-workspace-info.ui.test.tsx:333-370`: the button exists disabled with no
  descriptor, enables on a full match, one click -> one invoke, and each single-field
  mismatch (owner / generation / stream) disables it again.

### States and i18n

- `call.call` "Call"/"Позвонить" (`en.json:902`, `ru.json:910`); `info.openDirectMessages`
  "Message"/"Написать" (`en.json:940`, `ru.json:950`); errors `message.sendFailed`,
  `workspaceMessenger.runtimeUnavailable`, `e2ee.cryptoUnavailable`
  ("Encryption is unavailable for this chat or local key storage could not be opened.").
- Light + dark: same chip in both themes (`bg-card-bg`, `text-icon-active`,
  `text-text-secondary`, `disabled:opacity-50`) — the native `NativeTheme.qml` token map
  already carries every token; the existing ProfileChip is theme-driven.
- Empty state: no DM exists -> Call creates it on click. Loading: chip disabled while
  creating. Error: create failure clears pending + logs; E2EE without local key ->
  sidebar Call disabled / header shows `e2ee.cryptoUnavailable`; workspace without
  `meet_url` -> call affordances hidden (`chat-page-workspace.ui.tsx:4100`).

## Native design

### New module `packages/native/src/native-call-bridge.h/.cpp`

`workspace::DmCallBridge` (QObject), owned by `NativeController` and exposed to QML as
`controller.callBridge` (same pattern as `controller.calls`):

- `pendingPartnerUuid` (QString property, NOTIFY on every actual change):
  `setPendingPartnerUserUuid(uuid)` mirrors the store setter;
  `clearPendingPartner()` is a **no-op when already empty** (no write, no NOTIFY);
  `consumePendingPartner()` returns the uuid and clears it in one step (the handoff's
  "clear exactly once" primitive).
- `activeCallDescriptor` (QVariantMap: `ownerKey`, `runtimeGeneration` (qulonglong),
  `streamUuid`) via `setActiveCallDescriptor` / `clearActiveCallDescriptor`.
- `setViewedChat(ownerKey, runtimeGeneration, streamUuid, direct)` records the surface
  identity; `callSurfaceEnabled` is true only while the descriptor matches all three
  fields (`chat-page-workspace-info` selector port), `viewedChatIsDirect` mirrors the
  `directPrivate` shape.
- `invokeCallSurface()` emits `callSurfaceInvokeRequested()` only while
  `callSurfaceEnabled` (the reference hands the sidebar a callable `invoke` only under
  the same match; a mismatch means "undefined", i.e. not invocable). The controller
  connects the signal to the header start flow (`startCall()`), so header and sidebar
  run through one code path and share the one in-flight guard.

### Controller flows (`native-controller.cpp`)

- `callFromProfile(userUuid)` (profile Call chip): guards mirror `handleCall`
  (authenticated, uuid, not self). Reuses `resolveProfileDirectNavigation` (extracted
  helper shared with `openDirectChatFromProfile`, same AlreadyOpen / OpenDefaultTopic /
  MissingDefaultTopic / StreamMissing matrix as `domain.cpp`):
  missing-default-topic logs and returns before setting pending; pending is set via the
  bridge; already-open attempts the handoff immediately; open-default-topic reuses the
  #134 navigation (default topic select, or the parked `profileDirect` wait until the
  topics page verifies); stream-missing runs `startDirectChat(peer)` with
  `m_profileCallPeer` recorded so `profileCallPending` tracks the create in flight.
- `profileCallPending` property: true while a Call-triggered DM create is busy — the
  QML chip binds its `disabled` to it (reference `callPending`). Failed or uncertain
  direct-chat settlement (`finishDirectChat`) for that peer clears the pending partner
  (`:507`/`:513` port).
- Pending handoff `attemptPendingCallHandoff()` — the `:2300-2316` consumer port:
  fires on bridge `pendingPartnerUuidChanged` (reactive subscription `:863-865`), at the
  tail of `activateStream`, after topic selection / parked default-topic resolution.
  Requires: pending set, authenticated, `meetHost` configured, viewed stream private with
  `directUserUuid == pending`. It consumes the pending (clear-exactly-once) and calls the
  shared `startCall()` for the now-current DM. While the same DM stays open the consumer
  is inert because the pending is already null.
- Stale-send guards in `startCall()` (the `:2148-2255` port), shared by header and sidebar:
  - `m_callViewEpoch` increments whenever the viewed conversation identity
    (owner/stream/topic) changes (`activateStream`, `selectTopic`, sign-out). The invoke
    captures it; a completion whose epoch is no longer the live guard epoch is dropped
    silently — no open, no error, no guard reset (the newer invoke owns the guard). This
    reproduces the reference's React remount + `captureWorkspaceChatUiAction` behaviour
    for A->B->A navigation and for unmount without abort.
  - one in-flight guard per view epoch: a second invoke within the same epoch (header +
    sidebar double click) is a plain no-op -> exactly one invite.
  - runtime-generation change: `ApiClient::clearSession`/`setAccessToken` bump the
    generation, the transport already cancels in-flight requests ("Request cancelled
    because the session changed."), and the completion fails the captured owner/epoch
    checks -> nothing opens (AbortSignal port). Sign-out also clears the store, the
    bridge and `m_callPending` (already done for #161/#162 paths).
- Active-call descriptor registration (`:2264-2297` port) in `syncCallBridge()`, called
  at the tails of `activateStream` / `selectTopic`, after the meet-host fetch and on
  sign-in/sign-out: registers `{ ownerKey, api generation, streamUuid }` while
  authenticated, a conversation is open, the meet host is configured and the stream is
  not encrypted; clears it otherwise (self-clear semantics).
- E2EE refusal: `startCall()` on an encrypted stream without an available local send key
  (`canCompose()` false — no chat context, unknown key state or empty `m_chatSendKey`)
  sets `callError` to the reference `e2ee.cryptoUnavailable` text and sends nothing. The
  native client still has no programmatic encrypted send (#161 recorded deviation), so
  an encrypted stream never registers the descriptor: the sidebar Call button stays
  disabled for E2EE DMs, matching `right-panel-workspace-info.ui.test.tsx:333-370`'s
  disabled path; the with-local-key encrypted invite send remains the follow-up below.

### QML

- `qml/NativeUserProfile.qml`: the `call` chip activates — `disabled` binds to
  `controller.profileCallPending || controller.callTargetCryptoUnavailable(userUuid)`
  (partner's existing DM encrypted without a local key), the glyph tint becomes the
  reference `text-icon-active` like the sibling chips, and `onClicked` routes to
  `controller.callFromProfile(userUuid)`. Geometry stays exactly the measured chip.
- `qml/NativeChannelInfo.qml`: compact Call row `channelInfoCallButton` (phone glyph +
  `qsTr("Call")`) shown while the viewed conversation is a direct DM
  (`controller.isDirectConversation`), `enabled: controller.callBridge.callSurfaceEnabled`,
  click -> `controller.callBridge.invokeCallSurface()`. Matches the reference selector
  semantics: button present, disabled unless owner+generation+stream all match.
- `qml/Main.qml`: no new surface (the `headerCallButton` keeps `canStartCall` visibility,
  the reference header gate `:4100`); its click already routes to the same `startCall()`.

### Deviations recorded

- Bridge store logging: the reference `logStoreAction` debug lines are not ported; the
  native client has no store logger. Behaviour (values, no-op clear) is identical.
- `setInvokeDmCallFromProfileHandler`/`invokeDmCallFromProfile` seam: skipped, exactly as
  the issue directs (no production consumer at the sha).
- React remount semantics: the reference's per-mount refs/epoch (`:641`,
  `captureWorkspaceChatUiAction`) are replaced by the controller's monotonic view epoch;
  equality of the route triple is deliberately *not* sufficient for a completion to apply
  (the A->B->A test at `:5597-5665` proves the stale completion must stay dropped even
  when the route comes back).
- Runtime generation: the native `ApiClient` generation is the counterpart of
  `runtimeContext.runtimeGeneration`; abort is transport-level cancellation (the callback
  receives a cancelled result) plus the epoch/owner drop, instead of a fetch AbortSignal.
- Encrypted invite with a local key: the reference encrypts the invite through the live
  E2EE session. The native client still has no programmatic encrypted send (recorded in
  docs/NATIVE_DESKTOP_CALL_LAUNCH.md); so on native an encrypted stream never registers
  the sidebar descriptor even when a local key *is* available, and invoking the header on
  such a stream keeps the recorded inline refusal instead of sending. The issue's
  acceptance case (no session -> refused with `e2ee.cryptoUnavailable`, nothing sent) is
  implemented exactly. Closing this needs the encrypted-send entry point to accept a
  programmatic body (follow-up under the Calls epic).
- Profile Call chip extra disable: native additionally disables the chip when the
  partner's existing DM is encrypted without a local key (the issue's window
  acceptance), which the reference expresses via the sidebar disabled button instead of
  the chip; a click in that native state would otherwise produce an unavoidable error.
- Pending partner cleared also when a direct-chat create settles `uncertain` (not only on
  explicit failure/rejection): the native create state machine distinguishes "unknown"
  from "failed" and the pending must not leak across attempts either way.
- Sidebar placement: the reference renders the Call button inside the encrypted
  chat info panel (`right-panel-workspace-info.ui.tsx:841-866`). Native puts it
  in the DM chat-info header row as `channelInfoCallButton` (the native info
  drawer has no encrypted key panel); the gating rule
  (owner+generation+stream all-match, else disabled) is byte-identical to the
  reference selector. Because the native surface lives inside the direct info
  panel itself, leaving the DM takes the button away with the panel instead of
  leaving it visible-but-disabled.

## i18n

Reused (already catalogued): "Call"/"Позвонить", "Message"/"Написать", "Share".
New `NativeController::tr` string shipped in
`packages/native/translations/workspace_ru.ts`:
"Encryption is unavailable for this chat or local key storage could not be opened." ->
"Шифрование недоступно для этого чата или хранилище локальных ключей не может быть открыто."

## Tests

`native-native-controller` (`tests/native-controller-test.cpp`):
- bridge store contract: set/clear roundtrip + NOTIFY counts, clear no-op on empty (no
  notify), consume returns once and clears exactly once, invokeCallSurface gated by the
  three-field match with each single-field mismatch, viewedChatIsDirect mapping.
- profile Call with no DM: click -> direct-stream POST -> default topic open -> exactly
  one meeting-URL invite -> call opens after the send is applied; create failure (404->fail)
  clears the pending partner, leaves no call and the chip busy flag settles.
- profile Call already-open: pending consumed, call starts without navigation; the same
  DM staying open never re-fires (second handoff attempt no-ops).
- descriptor match: descriptor present -> `callSurfaceEnabled` true for the viewed chat,
  each single-field mismatch (owner / generation / stream) false; invoke routes one invite.
- shared in-flight guard: double invoke (header + sidebar) -> exactly one invite POST;
  deferred old send resolving after A->B->A navigation opens nothing and does not reset
  the newer guard (second click still sends its own invite once); session generation
  change during an in-flight send aborts it (no call opens).
- E2EE stream without a session: invoke refused with the `e2ee.cryptoUnavailable` text,
  no POST, no active call (extends the existing `encryptedStreamSendsNoPlaintextCallUrl`
  pin with the exact message assertion).

`native-native-window` (`tests/native-window-test.cpp`, Xvnc):
- other-user profile: Message | Call | Share row, Call enabled with reference geometry
  (radius 8 card, 40x40 glyph box, 30 px phone glyph, 12 px secondary label, opacity 1)
  in light and dark, label text "Call" (en) / "Позвонить" (ru).
- Call chip disabled: during the DM create (busy server) and for an encrypted partner DM
  without a local key (opacity 0.5, enabled false).
- Call click on a DM-less peer creates the DM and starts the call (assert via
  controller properties in the live scene).
- chat info drawer: `channelInfoCallButton` visible for a DM, enabled while the
  descriptor matches the viewed chat; disabled for an encrypted DM.

`native-native-locale`: catalog guard over the new ru entry (existing guard covers the
whole file; no case change needed beyond the catalog entry).
