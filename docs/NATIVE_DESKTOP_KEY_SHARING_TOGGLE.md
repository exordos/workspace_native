# Encrypted channel key-sharing toggle (issue #632)

Parity-preserving port. Oracle: workspace_ui `75bd21f37de94728e2ebe100acfea24ac9c1a4e1`.
Backend field: `allow_key_sharing_by_holders` (workspace_backend 8259c22).

## Reference measurements (workspace_ui @ 75bd21f)

- `entities/messenger/messenger-adapters.lib.ts:52-57`: when the stream DTO carries
  `allow_key_sharing_by_holders`, the model stores the value plus
  `keySharingUpdatedAt = dto.updated_at`. `ownerUuid = dto.owner`
  (adapters :37), `updatedAt = dto.updated_at` (:71).
- `entities/messenger/messenger-stream-key-sharing.lib.ts:3-10`
  `editableStreamKeySharing(stream, userUuid)` returns a value only when
  `stream.encryption === true && stream.ownerUuid === userUuid`; otherwise
  `undefined` (the row is not editable).
- Same file :12-25 `isNewerStreamKeySharingSnapshot`: confirmed-vs-snapshot compare
  parses whole milliseconds first and, on equality, compares the fractional
  digits padded to six places (Postgres can commit two updates inside one
  millisecond). :27-43 `preserveConfirmedStreamKeySharing`: a realtime snapshot
  may not clobber the confirmed value unless its `updated_at` is newer under
  that compare.
- `entities/messenger/messenger-sidebar-actions.lib.ts:600-632`
  `updateMessengerStreamKeySharing`: guards editability, PUTs
  `{ allow_key_sharing_by_holders }` to the stream (:608), then readback-verifies
  the echo (`uuid`, `project_id`, `user_uuid` owner and the requested flag,
  :614-621), keeps a newer confirmed value when the echo is stale (:622-624) and
  upserts `{ allowKeySharingByHolders, keySharingUpdatedAt: dto.updated_at }`
  (:626-643).
- `features/encrypted-chat-info/encrypted-chat-info.ui.tsx:399-421`: inside the
  Channel settings screen, between the read-only hint and the "Encryption keys"
  row, a `PanelMenuRow role="switch"` labelled `channel.allowMembersToShareKeys`
  with a right switch: track `h-5 w-9 rounded-full` (`bg-accent` when checked,
  `bg-border-subtle` otherwise), knob `h-4 w-4 rounded-full bg-white` at
  `left-0.5 top-0.5` translated `translate-x-4` when checked. In px: track
  36x20 r10, knob 16x16 at (2,2), checked knob x-offset 18. PanelMenuRow row
  height is 44 px (py-1.5 around min-h-8, same row the existing
  `protectedChatKeysRow` already ports).
- `features/encrypted-chat-info/encrypted-chat-info-connected.ui.tsx:231-275`
  `useChannelKeySharing`: row state exists only for encrypted streams; while a
  request for the scope (owner key + runtime generation + stream) is in flight
  the switch is disabled; the checked value commits from the server echo, and a
  failure with the scope still active shows `toast.error` with
  `channel.keySharingUpdateFailed`.
- Labels (`i18n/locales/en.json:795-796`, `ru.json:801-802`):
  EN "Allow members to share keys" / "Could not update key sharing.";
  RU "Разрешить участникам делиться ключами" /
  "Не удалось изменить настройку передачи ключей."

## States

The toggle is visible only for the stream owner of an encrypted stream
(issue #632 States + Acceptance "row hidden for non-owners"). The reference
keeps a disabled row for non-owners (`editableStream` -> `disabled`); the issue
states narrow the native surface to owner-only visibility, which this port
implements. Disabled while a toggle is in flight. Light+dark, ru+en.

## Figma

Token loaded; desktop page tree (`~/dev/reference_state/figma/tree.json`,
Exordos-Core `Q3BtN9qwfQV8rAsyBAgCy1`). The desktop "design макеты (desktop)"
sections (incl. Шифрованные чаты `10879:26030`) carry no key-sharing toggle
frame; the only match is the mobile section "Защищенный канал (с ключом
шифрования)" `13127:36323`, which the desktop client ignores per the lane
contract. Geometry therefore comes from the reference code measurements above.

## Native design

- DTO: `domain.h::Stream` gains tri-state `std::optional<bool>
  allowKeySharingByHolders` and `std::optional<qint64> keySharingUpdatedMicros`;
  `parseStream` sets both when (and only when) `allow_key_sharing_by_holders`
  is present, with `keySharingUpdatedMicros` from `updated_at` via the existing
  microsecond-precision `optionalUpdatedMicros` (six fractional digits - the
  exact resolution the reference ms+fraction compare uses, so one integer
  `>` compare reproduces the tiebreak).
- Snapshot precedence: in `applyRealtimeEvent` (stream.created/updated/read)
  the parsed row is passed through `preserveConfirmedKeySharing` before merge:
  if the existing row has a confirmed value and the incoming event's
  `keySharingUpdatedMicros` is absent or not strictly newer, the confirmed
  value and its timestamp are carried into the merged row.
- Controller: `chatKeySharing` QVariantMap projection (`checked`, `disabled`;
  empty when the row must be hidden: not the channel stream, not encrypted,
  DTO without the field, or the DTO's `owner` differs from the current user -
  the reference `ownerUuid = dto.owner` comparison, kept independent of the
  optional crypto witness), `chatKeySharingError` string, and
  `setChatKeySharing(bool)` mirroring `updateStreamNotificationMode`:
  token-guarded in-flight state, `PUT messenger/streams/{uuid}` with
  `{"allow_key_sharing_by_holders": allow}`, readback verification of the echo
  (uuid, owner, requested flag), commit `*row = parsed` on success - unless a
  local confirmation is stamped strictly newer than the echo, in which case the
  stale echo is dropped silently (reference :622-624 status "applied") - and on
  failure keep the previous value and set
  `tr("Could not update key sharing.")`; after an ambiguous failure (transport
  loss or 5xx) the stream is reconciled through a targeted `GET
  messenger/streams/{uuid}` so a committed write self-heals without rebuilding
  the open drawer (a token-renewal reply still settles through the catalog). All notifications reuse `stateChanged`
  (no new NOTIFY signals - binding-budget constraint from e2ee-read cases).
- UI: `qml/NativeEncryptedChatInfo.qml` settings screen gets
  `protectedChatKeySharingRow` (Button, 44 px, hover token
  `sidebar-item-hover`, `Accessible.role: Accessible.Switch`) with label
  `protectedChatKeySharingLabel` and switch `protectedChatKeySharingSwitch`
  (36x20 r10, `accent`/`border-subtle`) + knob `protectedChatKeySharingKnob`
  (16x16 white at x 2 / 18). Failure renders the reference toast text as the
  inline notice row pattern this codebase already uses for channel-info action
  failures (`channelInfoNotificationError` precedent, `notice-base` token),
  objectName `protectedChatKeySharingError`.
- i18n: `translations/workspace_ru.ts` entries in the `workspace::NativeController`
  (toast text) and `NativeEncryptedChatInfo` (label) contexts.
- Test placement: controller cases (DTO parse/editability, optimistic toggle +
  rollback + error notice, snapshot-vs-confirmed precedence) in
  `tests/native-controller-encrypted-channels-test.cpp`; window cases (row
  hidden for non-owners, switch geometry, toast text on failure) in
  `tests/native-e2ee-read-test.cpp`, the window-fixture group that owns the
  encrypted chat info drawer scenes (precedent: issues #242-#245).
