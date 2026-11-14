# Native encrypted channel creation

This slice enables the Protected channel (E2EE) path in the native create chat
dialog. The switch was previously a permanently disabled placeholder
(`docs/NATIVE_DESKTOP_CHANNEL_CREATION.md` recorded encrypted bootstrap as a
separate feature). It is now interactive and drives a real encrypted channel
creation through the same creation workflow, phases, authority fences and
navigation as the ordinary path. The sibling
[ordinary channel creation slice](NATIVE_DESKTOP_CHANNEL_CREATION.md) remains
the base contract; this document only records the encrypted delta.

## Reference contract

Measured from the React reference `workspace_ui` at
`230e02b8b0273dee556a406746d122180a68b3e7`:

- `packages/web/src/features/create-chat/create-chat-dialog.ui.tsx:401-421`:
  a `role="switch"` input with `aria-label` `channel.createEncryptedChat`
  (en "Create encrypted chat", ru "Создать шифрованный чат") and the visible
  label `channel.protectedChannelE2ee` (en "Protected channel (E2EE)",
  ru "Защищенный канал (E2EE)"). The track is `h-[18px] w-[30px] rounded-full`
  with `bg-border-subtle` unchecked and `peer-checked:bg-accent` checked
  (the reference `--color-accent` is `#7087ff`, which is exactly the native
  `blue-cold` palette `accent` token). The knob is
  `h-[14px] w-[14px] rounded-full bg-white` at `left-0.5 top-0.5` (x/y = 2)
  and shifts `translate-x-3` (12 px, x = 14) when checked. The switch is
  `disabled` while a creation is in flight; the disabled label uses 60%
  opacity.
- `create-chat-dialog.hook.ts` behaviour contract (asserted by
  `create-chat-dialog.hook.test.ts`):
  - "clears and disables conflicting options when encrypted creation is
    selected": turning the switch on clears invite-only, announce and
    announcement-only; while it is on those setters cannot re-check them.
  - "keeps one encrypted intent across repeated clicks and dialog
    close/reopen": one per-owner attempt object survives repeated Create
    clicks and dialog close/reopen; the same object is passed to every retry,
    so one intent never forks a second POST or key material.
  - "creates an encrypted channel through the normal success navigation and
    clears exclusive flags": success navigates like the ordinary path and the
    encrypted selection resets afterwards.
  - "ignores a saved create callback after its runtime is replaced before
    rerender": an intent captured on a replaced runtime must not act.
  - "keeps failed creation visible and does not navigate or close": a failed
    creation keeps the dialog open with an error line
    (`channel.createFailed`, `channel.encryptionUnavailable`,
    `channel.encryptionMismatch`, `channel.creationUncertain`).
- The encrypted creation action
  (`entities/messenger/messenger-create-chat-actions.lib.ts` plus
  `chat-initial-key-bootstrap.lib.ts` and `chat-real-key-bootstrap.lib.ts`)
  runs one frozen attempt per intent with phases
  `idle -> prepared -> dispatched -> resolved`:
  1. `prepare()` creates the initial group key locally (32-byte key plus a
     fresh key UUID and creation id) and stages it before the POST; the wire
     only ever carries the public key UUID.
  2. The stream POST carries `encryption: true` and
     `current_encryption_key_uuid: <prepared key uuid>` with `invite_only`
     and `announce` forced to `false`.
  3. The returned bundle must echo `encryption === true` and the same key
     uuid, otherwise `encryption-mismatch` and no navigation.
  4. `finalize()` commits the staged key to storage bound to the created
     stream UUID before members are added.
  5. An ambiguous dispatch is reconciled GET-only (stream pages filtered by
     owner, native source, non-direct, `encryption: true` and the same key
     uuid, then the default topic page); the POST is never replayed.
  6. A concurrent second call on the same pending attempt fails with
     `creation-uncertain`; a changed input identity after dispatch/receipt
     also fails with `creation-uncertain`.

## Backend request shape (verified)

Verified against the audited backend docs
`workspace_backend/docs/en/workspace_api.md` (Streams / "Stream encryption"):

- `encryption` is a creation-only boolean, immutable afterwards.
- `current_encryption_key_uuid` is required at creation when `encryption` is
  `true` (absent or `null` when `false`) and afterwards changes only through
  a `key_announced` message from the owner. `PUT` rejects both fields.
- The backend stores only the key UUID, never key material.

The native encrypted stream POST is therefore the ordinary body with the
encryption pair filled in:

```json
{
  "uuid": "one immutable attempt UUID",
  "name": "trimmed name",
  "description": "trimmed description",
  "source_name": "native",
  "invite_only": false,
  "announce": false,
  "encryption": true,
  "current_encryption_key_uuid": "one immutable generated key UUID"
}
```

`POST /api/workspace/v1/messenger/streams/`. No additional key request is
sent: the group key is local-only (per the crypto doc, the backend knows the
generation UUID and nothing else).

## Figma oracle

Desktop canvas, Messenger section `2473:13434`: the create-channel frames
`4284:21364` ("Создание канала") and `4284:22074` ("Cоздание канала (2)")
contain the create chat dialog without the Protected channel toggle - the
Messenger section does not carry an encrypted variant. The Protected channel
create design lives in the desktop canvas "Шифрованные чаты" section, frames
`10879:26911` ("Создание защищенного чата") and `11026:27501` ("Создание
защищенного чата (цвета из палитры)"). Figma node detail for those two
frames could not be fetched during this implementation run: the Figma REST
API returned `429 Rate limit exceeded` repeatedly (the token itself is
available via `figma-token`, and the section/frame ids above were resolved
from the cached tree at `~/dev/reference_state/figma/tree.json` and a
successful earlier pull of the Messenger frames). Geometry and colors in
this document are therefore quoted from the React reference source, which is
the binding acceptance oracle for this parity slice.

## Native design

### Controller (`packages/native/src/native-controller.*`)

`createChannel(name, description, inviteOnly, announce, memberUuids,
encrypted)` gains a trailing `encrypted` argument (default `false`, so every
existing call keeps ordinary behaviour). With `encrypted = true`:

- Admission keeps every ordinary validation (trim/Unicode bounds, directory
  checks, retention caps). An encrypted intent forces `inviteOnly` and
  `announce` to `false` in the frozen input, matching the reference
  "clears exclusive flags" contract.
- At admission the controller generates the initial key material locally:
  `NativeChatCrypto::createGroupKey()` (32 bytes) plus one fresh key UUID,
  stored on the intent. If the OS crypto provider or the attached chat key
  vault is unavailable, creation is rejected locally before any request with
  "Encrypted channel creation is not available yet." and no POST is sent.
- The stream POST uses the body above. The attempt UUID stays immutable for
  the intent exactly as in the ordinary path, and there is still exactly one
  POST per admitted intent: repeated clicks while busy or unresolved are
  ignored, and `checkChannelCreation()` remains GET-only.
- Response validation mirrors the ordinary DTO gate through
  `creationStreamDto(..., ordinary=false)` plus explicit identity checks and
  the encrypted echo requirement: `encryption === true` and
  `current_encryption_key_uuid === <frozen key uuid>`. A POST response or a
  canonical GET that does not echo those fields ends the intent as
  `uncertain` with "The server did not confirm encrypted channel creation.
  Check status before trying again." (no navigation, dialog stays open).
- The vault commit (the `finalize` step) happens once, inside
  `verifying-stream`, after the canonical GET for the same UUID has validated
  and before the default-topic/members/catalog stages:
  `NativeChatKeyVault::put(lease, {streamUuid, keyUuid}, groupKey, 0)`. A
  failed commit ends the intent `uncertain` with
  "Could not store the encrypted channel key. Check status before trying
  again." so an explicit GET-only Check can retry the local write; no stream
  POST is ever replayed. After the key is stored the in-memory group key bytes
  are cleansed from the intent.
- Deviation from React staging order: the reference stages the key before the
  POST because its storage accepts a creation id without a stream. The native
  vault key address binds `stream + generation`, so the native commit can only
  happen after the stream UUID is authoritative. The observable contract
  (key never crosses the wire, exactly one committed key per created stream,
  failure keeps the dialog open) is preserved.
- `channelCreationForm()` additionally exposes `encrypted`, which is what the
  QML layer uses to restore one immutable encrypted intent across dialog
  close/reopen for unresolved phases (the existing unresolved-form retention
  in `m_channelCreations` is the state home; the reference's attempt map and
  the native retained form are the same single-intent idea).
- Replaced-runtime protection is unchanged and covers the new stages: every
  async continuation passes `channelCreationContext()` (API generation,
  owner/project, route, catalog, cache lease), so a replaced runtime cannot
  publish, navigate or write the vault.

### QML (`packages/native/qml/Main.qml`)

- The disabled placeholder becomes an interactive CheckBox-rendered switch
  (`channelEncryptionSwitch`): 30x18 track, radius 9, 14 px white knob at
  x = 2 unchecked / x = 14 checked, track `border-subtle` unchecked and
  `accent` checked, label "Protected channel (E2EE)", accessible name
  "Create encrypted chat", 60% opacity when disabled. The placeholder tooltip
  "Encrypted channel creation is not implemented yet" is removed.
- The switch is disabled while the channel creation form is frozen (creation
  in flight, including `uncertain`/member-recovery phases), matching the
  reference `disabled={vm.creating}` plus the native freeze contract.
- Turning the switch on clears the invite-only and announce checkboxes; while
  it is on those two options are disabled (announcement-only was and stays a
  disabled placeholder). This matches "clears and disables conflicting
  options"; the reference enforces the same rule by gating the setters.
- On successful creation the dialog closes and navigates exactly like the
  ordinary path (aggregate navigation plus the unchanged choose-topic
  prompt); a failed creation keeps the dialog open with the error line.

## Local verification

`native-controller-test.cpp` (group `native-native-controller`) covers: the
exact encrypted create request shape including the non-null key uuid and
forced `false` exclusive flags; one immutable attempt uuid and one immutable
key uuid across repeated clicks; the vault commit of the generated key for
the created stream; an encryption echo mismatch ending `uncertain` without
navigation; a definite POST failure keeping the intent retryable with a new
uuid; and a replaced runtime (logout during the POST) publishing nothing.

`native-window-test.cpp` (group `native-native-window`) covers: the switch is
present, enabled and interactive; turning it on clears and disables
invite-only and announce; success navigates to the new stream aggregate with
the topic prompt; an ambiguous creation keeps one intent across dialog
close/reopen (one POST, same attempt); a failed creation keeps the dialog
open without navigation.

```sh
ctest --test-dir build -R '^native-native-(controller|window|locale)$' --output-on-failure
```

Deployed backend acceptance of encrypted creation is verified separately
(live workspace). Encrypted sending, key requests/grants for other devices,
rotation and key management remain governed by
[the crypto acceptance doc](NATIVE_DESKTOP_CRYPTO.md).
