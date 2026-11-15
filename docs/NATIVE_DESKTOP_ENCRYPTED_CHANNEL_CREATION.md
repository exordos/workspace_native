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
  key address binds `stream + generation`, so the key COMMIT can only happen
  after the stream UUID is authoritative. Durability is still closed before
  dispatch through a separate pre-dispatch stage record (next bullet). The
  observable contract (key never crosses the wire, exactly one committed key
  per created stream, failure keeps the dialog open) is preserved.
- Durable pre-dispatch stage (review findings 4574928575/4574928581): before
  the stream POST is dispatched the controller writes
  `NativeChatKeyVault::putCreateStage(lease, {attemptUuid, keyUuid, groupKey,
  createdAt})`. The attempt uuid is the idempotent stream uuid the POST body
  carries, so the pair `(attempt, key uuid)` is the complete recovery
  identity. If the stage write fails, creation is rejected locally before any
  request ("Encrypted channel creation is not available yet."). The stage is
  cleared only after the canonical GET echo verified and the key committed
  (in `commitChannelKey`, best-effort). Recovery: the first authenticated use
  of a vault (controller init with an attached vault, vault open completing
  after login, or a login with an already-open vault) scans
  `createStages(lease)` once per owner and vault lease, skips attempts that
  still have a live in-memory intent, and for each pending stage performs the
  canonical `GET /streams/<attempt>`: a verified encrypted echo commits the
  key and clears the stage; a `404` or any other non-committal answer keeps
  the stage (see the next bullet) for bounded in-session rescans and for the
  next controller init. The staged key never appears in logs or QML and is
  cleansed after use.
- Two-phase dispatch contract (review findings 4575672241/4575672242): the
  durable stage carries a phase. `putCreateStage` writes it as `prepared`.
  The controller durably marks the stage `dispatch-attempted`
  (`markCreateStageDispatchAttempted`) immediately BEFORE the stream POST is
  handed to the network stack, so the phase distinguishes a stage whose POST
  provably never left the process from one whose POST may still land
  server-side. Recovery retires a `prepared` stage directly — no GET, no
  retention — reclaiming its slot in the vault's 32-stage create budget;
  without this, a run of process losses inside the stage-to-dispatch window
  would permanently exhaust the budget and starve every later encrypted
  create, restarts included. If the dispatch marker itself cannot be written,
  creation is rejected locally and the stage cleared on the spot: at that
  point the POST has provably not been dispatched and never will be, so the
  reclaim is safe. Dormant quarantine (review finding 4576535469, which
  replaces the earlier conclusive-absence reclaim): a crash between the
  durable marker and the actual socket write leaves a `dispatch-attempted`
  stage whose POST also never left, and whose stream answers `404` to every
  recovery GET forever — retention alone could never remove it. But a `404`
  proves absence now, not a terminal outcome: a POST that was dispatched
  but whose response was never observed may still commit server-side
  arbitrarily late, and this client can observe no deadline or cancellation
  for the attempt. Because a `dispatch-attempted` stage can hold the only
  copy of the group key, recovery must never destroy it on absence evidence
  alone — an earlier revision retired such stages after five definitive
  `404` observations across separate cycles once the stage aged past 10
  minutes, and that reclaim deleted the key of a channel whose POST landed
  late. Now each scan cycle that gets a DEFINITIVE `404` durably bumps the
  stage record's `missingEvidence` counter (a `2xx` answer — the stream
  exists — resets it to `0`; transport errors and `5xx` prove neither
  presence nor absence and leave it untouched), and the counter saturates
  at `minimumChannelStageMissingObservations`. A stage at saturation is
  DORMANT: its group key and attempt identity stay in the same durable
  stage record, inside the same bounded 32-record create budget — dormant
  records are ordinary stage records, never a separate quota and never
  evicted — and the bounded backoff rescans keep re-checking them. When a
  later cycle's GET finally serves the stream, the verified echo commits
  the verified echo commits the key and frees the slot (re-adoption). The residual starvation bound
  this design accepts: 32 dispatched-but-unresolved attempts hold the whole
  budget and a later encrypted create is rejected locally until one of the
  dormant streams lands OR the user explicitly closes an attempt through the
  user-invokable terminal resolution (next bullet) — the only alternative,
  automatically evicting dormant keys, would restore exactly the key loss this
  finding reported. The mechanism
  keeps the R2 retention contract closed: a POST still in flight or still
  committing is never reclassified — in-session the live intent is skipped
  by the scan and only the POST's own definite rejection (400/401/403/404)
  or the verified echo closes it, and after a restart a late commit is
  found by the re-arming rescans, where the first verifying `2xx` commits
  the key and clears the stage. Stage records written before this contract existed
  carry no phase and decode as `dispatch-attempted` with zero evidence (the
  same never-lossy default).
- User-invokable terminal resolution (review finding 4576823403, R7): dormant
  quarantine removed the key-loss bug but left a permanent dead end — a full
  32-record budget of dormant attempts rejects every later encrypted create
  across restarts, and no automatic mechanism may ever free a slot because
  absence evidence never proves a terminal outcome. The budget must therefore
  never be permanently starved, and the safe terminal authority a client
  cannot be is a HUMAN. The shape chosen: (1) an unresolved-attempt inventory
  on the controller, `unresolvedChannelCreateStages()` — one row per durable
  stage not owned by a live in-memory intent, carrying the attempt identity
  (the uuid-attempt and the staged key uuid), the created timestamp, the
  dispatch phase, the absence evidence count and the DORMANT flag — and never
  the group key; (2) an explicit user-invokable operation
  `resolveChannelCreateStage(attemptUuid)` (both `Q_INVOKABLE`, so any UI can
  drive them without new plumbing; the full settings page that presents them
  is out of scope for this slice and deliberately not built here), which
  re-checks the attempt's stream identity NOW and only ever acts on the
  combination of the human's explicit request plus a DEFINITIVE live answer,
  reporting the terminal result through the `channelCreateStageResolved
  (attempt, outcome, message)` signal:
  - `released` — the canonical GET answers a definitive `404`: the stream does
    not exist and the human, not an absence heuristic, decides the attempt is
    not worth waiting for any longer. The stage record is cleared and its
    budget slot is free again; the key discard is the explicit consequence of
    that human order, never inferred by the software. A never-dispatched
    `prepared` stage is also released without a GET (its POST provably never
    left any process — the same reclaim the recovery scan performs).
  - `committed` — the stream now exists and the encrypted echo verifies under
    the staged attempt/key uuid: the key is COMMITTED to the vault (the same
    re-adoption commit recovery performs) and the stage closed. A resolve can
    therefore never discard the key of a channel that turned out to exist.
  - `unresolved` — anything inconclusive (transport error, `5xx`, a `2xx` echo
    that does not verify, an unknown attempt, a live in-flight intent for the
    attempt, a failed vault write, or the session/vault lease changing
    mid-check): the stage record and its key stay byte-for-byte intact (a
    non-verifying `2xx` only voids the stored absence evidence, exactly like
    the recovery scan).
  An encrypted create rejected because the budget is full (`Capacity` from the
  vault at staging time) now names the condition and points at this path
  ("All encrypted channel creation slots are held by unresolved attempts...")
  instead of the generic unavailability line; dormants stay bounded inside the
  same 32-record budget as in R6 — no automatic aging was reintroduced.
  R4-style starvation now breaks via explicit user resolve, not automatic
  eviction.
- Recovery resolution rules (review findings 4575117257/4575117265/4575117269):
  a `404` from the recovery GET is NOT proof the original POST cannot land —
  a delayed or in-flight creation can still commit server-side moments after
  that GET — so the stage is retained on every non-committal answer (404,
  transport error, non-verifying echo) and only the live POST's own definite
  rejection (400/401/403/404) closes it as "definitively not created" (this
  also keeps rejected attempts from accumulating against the vault's
  create-stage budget and starving later encrypted creates). A scan pass that
  ends with any stage unresolved (vault read failure, failed GET, 404,
  non-verifying echo) schedules a bounded in-session rescan — at most 5
  fast retries, 1 s apart, per owner+vault-lease. Once that fast window is
  exhausted with stages still unresolved the scan does NOT go silent for the
  lease: it re-arms with exponential backoff capped at 30 s per cycle (each
  re-arm is a fresh scan cycle), so an outage longer than the window still
  heals without a controller restart; a pass with nothing unresolved stops
  arming, so an idle vault never polls. A
  `prepared` (never-dispatched) stage never reaches this retention rule: it
  is retired by the two-phase dispatch contract above before any request.
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
uuid; a replaced runtime (logout during the POST) publishing nothing; and the
durable crash window: with the POST reply deferred, destroying the controller
after dispatch and creating a fresh controller on the same vault recovers and
commits the key (verified echo), keeps the stage uncommitted (mismatched
echo), and re-verifies nothing once the stage was cleared by the commit. The
recovery-race findings add: a definite POST rejection clearing the durable
stage (`encryptedChannelCreationDefiniteFailureClearsStage`); a restart whose
recovery `404` races the still-committing POST keeping the stage and then
committing it from a bounded in-session rescan
(`encryptedChannelCreationRecoveryKeepsStageWhileStreamMissing`); and a
transient recovery failure being retried within the same vault session
(`encryptedChannelCreationRecoveryRetriesTransientFailureInSession`). The
two-phase dispatch findings add: 32 crash-before-dispatch stages being
reclaimed by recovery so a later encrypted create can stage again, without
any recovery GET for the never-dispatched attempts
(`encryptedChannelCreationCrashBeforeDispatchStagesCannotStarveLaterCreates`);
and the guard that a dispatched stage whose recovery sees `404` is retained,
never retired (`encryptedChannelCreationDispatchAttemptedStageSurvivesRecoveryScan`).
The R6 dormant quarantine (finding 4576535469) removes the absence-based
retire: a dispatch-attempted stage is never deleted on 404 evidence alone —
it saturates its durable absence counter, stays DORMANT with its key inside
the bounded create budget, and is re-adopted when its late stream finally
lands (`encryptedChannelCreationDispatchAttemptedKeySurvivesConclusiveAbsence`;
`encryptedChannelCreationDispatchAttemptedStagesGoDormantWithoutKeyLoss`
additionally pins the accepted residual bound: a dormant-full budget
rejects a new encrypted create locally rather than evicting any key, and
after re-adoption creates complete again); recovery re-arms with bounded
backoff after the fast retry window ends
while the failure is still in effect, so an outage longer than the window
still commits the stage in-session
(`encryptedChannelCreationRecoveryReArmsAfterFastRetryWindowEnds`).
The R7 user-invokable terminal resolution (finding 4576823403) adds
`encryptedChannelCreationDormantBudgetIsUserResolvable`: a dormant-full budget
still rejects a 33rd encrypted create locally, the unresolved-attempt
inventory lists every dormant attempt with its identity, created timestamp and
dormant flag (never its key), resolving a definitively-404 attempt releases
the record and frees the slot so the next encrypted create completes, and
resolving an attempt whose stream has landed commits (re-adopts) the key
instead of discarding it; resolving an unknown attempt changes nothing.

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
