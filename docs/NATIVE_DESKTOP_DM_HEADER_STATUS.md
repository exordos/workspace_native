# Direct-chat header: status caption with typing, custom status and last seen (issue #88)

Reference: workspace_ui at `61dacef78dab2f5d0be76f283bebf49fe29b84af`.
Figma note: `FIGMA_TOKEN` has returned HTTP 401 since 2026-11-16, so this slice
uses the pinned reference source as the design oracle (accepted precedent from
the deactivated-badge slice below and issues #222/#243).

## Reference contract

- `packages/web/src/widgets/chat-view/chat-header.lib.ts:20-27`
  `resolveDmStatusText` priority chain, evaluated in this order:
  1. `isAccountDeactivated` -> `t("dm.partnerBlocked")` ("Account deactivated").
  2. `isTyping === true` -> `t("chat.typing")` ("Typing").
  3. `customStatus` non-null -> shown as-is (custom status with its emoji).
  4. otherwise `resolveDmPresenceText` (`chat-header.lib.ts:4-18`):
     `presenceState === "active"` -> `t("presence.online")` ("online");
     `presenceState === "idle"` -> `t("presence.away")` ("away");
     `lastSeen == null` -> `t("presence.offline")` ("offline");
     `lastSeen === t("presence.online")` -> `t("presence.online")` ("online");
     otherwise `t("presence.lastSeen", { time })` ("last seen {time}").
- `packages/web/src/widgets/chat-view/chat-header.types.ts:1-17`
  `ChatHeaderDmPartner`: `presenceState` `null` means presence data is
  unavailable (it is not "offline" data, but the offline caption falls out of
  the `lastSeen == null` branch); `lastSeen` is the *display string* the
  partner's last-seen text shows while offline ("Last-seen text shown while
  the partner is offline"); `customStatus` is the combined custom status with
  emoji; `isTyping` shows a temporary typing state instead of presence.
- `packages/web/src/widgets/chat-view/chat-header-direct.ui.tsx:43-55`
  the partner block: 40 px avatar, name `h1 text-sm font-semibold`, caption
  `span text-xs text-text-muted truncate`; the whole block is one overlay
  button (`chat-header-direct.ui.tsx:7-8,47-55`) with
  `aria-label=t("a11y.openUserProfile")` ("Open profile: {name}") that opens
  the partner profile.
- Locales at that sha (verbatim, `en.json`/`ru.json`):
  - `dm.partnerBlocked`: "Account deactivated" / "Пользователь заблокирован"
  - `chat.typing`: "Typing" / "Печатает"
  - `presence.online`: "online" / "В сети"
  - `presence.away`: "away" / "Отошёл"
  - `presence.offline`: "offline" / "Не в сети"
  - `presence.lastSeen`: "last seen {{time}}" / "был(а) {{time}}"
  - `a11y.openUserProfile`: "Open profile: {{name}}" / "Открыть профиль: {{name}}"
- The presence dot, avatars and the deactivated badge itself are covered by
  issue #4 and issue #177 (`NATIVE_DESKTOP_DM_HEADER_DEACTIVATED.md`); this
  slice only completes the caption text and the partner-block click target.

## Reference data pipeline (important caveats)

As with `isAccountDeactivated` (documented in
`NATIVE_DESKTOP_DM_HEADER_DEACTIVATED.md`), at the pinned sha the production
selector `selectWorkspaceChatHeaderView`
(`packages/web/src/entities/messenger/messenger-chat-header.lib.ts:117-126`)
assigns only `name`, `avatarUrl` and `presenceState` on `dmPartner`.
`lastSeen`, `customStatus` and `isTyping` are declared and consumed by the
header contract but are set only in reference tests:

- `isTyping` is fed by the typing indicator store
  (`packages/web/src/features/typing-indicator/typing-indicator.model.ts`,
  entries expire after 15 s), whose realtime router
  (`typing-event-routing.ts`) has no production consumer at this sha.
- `lastSeen` has no upstream producer at all at this sha; the contract fixes
  it as a server-provided display string.

The native client reduces exactly these realtime event kinds:
`message.created/updated/deleted/read`, `stream.created/updated/deleted/read`,
`topic.created/updated/deleted/read` and `user.updated`
(`src/native-controller.cpp`, `applyRealtimeEvent`). None of them carries a
typing flag, so the native client has no wire source for typing today. This
slice implements everything the wire provides and records the typing gap on
issue #88; nothing is invented.

## Native design

### Controller projections (`src/native-controller.{h,cpp}`)

- `UserPresentation` gains `QString lastSeen`, parsed in
  `makeUserPresentation` from the optional `last_seen` string field of the
  same authoritative user directory rows used for presence/status/avatar.
  Snake_case naming follows `account_deactivated` (#177): the reference names
  no upstream field for this fact, so only this projection changes when the
  real API names it. Missing/non-string -> empty, i.e. contract `null`
  (never invented). Like the presence/status facts it participates in the
  `updated_at` ordering guard: an older row keeps the newer last seen.
- `userLastSeen(id)` accessor mirrors the `userPresence` gating
  (authenticated + authoritative channel-directory owner).
- `directPeer()` additionally exposes:
  - `customStatus` = `userStatusLabel(id)` - the existing combined
    `status_emoji + ' ' + status_text` label (legacy shortcode mapping
    already applied), the native `customStatus` input of the contract.
  - `lastSeen` = `userLastSeen(id)`.
  - `typing` is deliberately **not** projected: no handled realtime event
    carries it. The QML caption chain still evaluates the typing branch
    against `directPeer().typing === true` (absent -> `false`), so the
    contract is complete and the branch activates as soon as a wire event
    exists; until then no test drives it from the mock, per the
    do-not-fabricate rule.

### QML (`qml/Main.qml`, `directConversationHeader`)

- New `directStatusCaption()` mirrors `resolveDmStatusText` in the same
  order using `directPeer` facts:
  `accountDeactivated -> qsTr("Account deactivated")` (reuses the #177
  entry); `typing === true -> qsTr("Typing")`; non-empty `customStatus`
  as-is; otherwise the presence chain
  `qsTr("online")/qsTr("away")/qsTr("offline")` (existing `presenceLabel`
  entries) and `qsTr("last seen %1").arg(lastSeen)` with the reference
  equality check against the online label first.
- `directPresenceStatus` keeps its objectName and geometry; only its `text`
  binding becomes `directStatusCaption()`. An unknown user (`presence`
  empty -> contract `presenceState null`) shows the offline caption, not
  online (native note already recorded above this binding).
- Partner block click target: mirroring the reference overlay
  (`chat-header-direct.ui.tsx:47-55`), a `MouseArea` named
  `directPartnerProfileAction` covers the whole partner block
  (avatar + name + caption), visible/enabled only when the direct peer id is
  known, `cursorShape: Qt.PointingHandCursor`, `Accessible.role:
  Accessible.Button`, `Accessible.name: qsTr("Open profile: %1").arg(name)`
  (reference `a11y.openUserProfile`), and one click opens the existing
  partner profile drawer through `rightDrawer.openWorkspaceUserProfile(peer)`
  (#134). The reference renders the overlay only with a profile handler and
  the native peer id is the handler precondition. The #134 side-panel
  information button keeps its behavior.

### Strings (`translations/workspace_ru.ts`, `Main` context)

| source | translation |
| --- | --- |
| `Typing` | `Печатает` |
| `last seen %1` | `был(а) %1` |
| `Open profile: %1` | `Открыть профиль: %1` |

`online` / `away` / `offline` / `Account deactivated` already carry the exact
reference wording in the catalog. The locale catalog guard covers the new
entries.

## Acceptance

- `native-controller-test` (group `native-native-controller`): `directPeer()`
  projects `customStatus` and `lastSeen` from the authoritative users rows
  (catalog + `user.updated` realtime), absence semantics (no field -> empty /
  contract null), and the `updated_at` ordering guard for the new fact.
- `native-window-test` (group `native-native-window`), data-driven en + ru on
  the direct-chat fixture asserting the rendered caption text:
  deactivated beats everything; custom status "On call" overrides presence;
  active/idle presence labels; offline without last seen -> offline caption;
  offline with last seen "15:30" -> "last seen 15:30"; a last seen equal to
  the online label renders the online label; unknown user shows the offline
  caption, never online. A separate case clicks the partner block and
  requires the profile drawer layer for the peer. The typing branch has no
  mock drive path (no wire event); this is recorded on the issue instead of
  being fabricated.
- `native-locale-test`: catalog guard for the three new strings.

## Live verification plan

`live_qa.py native-login` screenshot + vision stage check; open a direct chat
and capture the header caption showing the presence caption the live wire
provides; if no live counterpart carries a custom status, prove the ordinary
caption live and state that the remaining branches are mock-proven at window
level; health p95 and send-latency gates per the standard live gate.
