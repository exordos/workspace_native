# Encrypted chat key management: key screen and saved-keys screens (issue #244)

Part of #242. Reference contract measured at `workspace_ui` commit
`230e02b8b0273dee556a406746d122180a68b3e7`. Builds on the protected drawer
(#243, `docs/NATIVE_DESKTOP_E2EE_CHAT_INFO.md`), whose `key` / `local-key`
screen scaffolds inside `qml/NativeEncryptedChatInfo.qml` this issue replaces
with the real screens.

## Reference behaviour

Screens mount inside the same drawer shell (`screen = "key" | "local-key"`,
`encrypted-chat-info.ui.tsx:522-632`); the body sits in a `ScrollArea` with a
20 px block gap (`gap-5 py-4`).

Key screen - `KeySummary` (`encrypted-chat-info.ui.tsx:87-129`):

- `section className="space-y-3 px-4"` (12 px between blocks, 16 px inset).
- Label row `flex items-center justify-between gap-2 text-sm text-text-muted`:
  `e2ee.keyUuid` + ghost copy button. Copy button (`ICON_BUTTON_CLASS`
  `ui.tsx:25`): `h-6 w-6 shrink-0 rounded p-0 text-accent`, `copy` icon 24,
  aria-label `e2ee.copyKey`, disabled while `loading`.
- UUID chip `truncate rounded-lg bg-card-bg px-2 py-1.5 text-sm leading-5`
  (radius 8, 8 px side / 6 px vertical padding, 20 px line, elided).
- `createdLabel` line `text-sm leading-4 text-text-muted`.
- No key: muted `e2ee.keyUnavailable` line at `text-sm`.
- `canRotateKey` (from `crypto.session?.getRights().canRotate`,
  `encrypted-chat-info-connected.ui.tsx:361`): fullWidth button
  `e2ee.rotateKey` with leading `./assets/cached.svg` 20 px icon, disabled
  while `loading || actionsPending`.

Copy action (`encrypted-chat-info.hook.ts:69-104`): clipboard write of the key
UUID guarded by the runtime scope; failure toast `e2ee.copyFailed`.

`createdLabel` source: `keyCreatedAt = t("e2ee.keyCreatedAt", {date:
formatDateJoined(keyCreatedAt)})` (`encrypted-chat-info-connected.ui.tsx:221`)
where `keyCreatedAt` is the earliest `key_announced` message date for the
current key UUID among loaded messages
(`encrypted-chat-info-selector.lib.ts:88-96, 113`). `formatDateJoined` =
`{year: numeric, month: short, day: numeric}` (`shared/lib/datetime.lib.ts:16`).

Local keys section (`encrypted-chat-info.ui.tsx:130-230`):

- `section className="space-y-2 border-t border-border-subtle pt-3"` - 12 px
  top border inset above the section, 8 px between blocks.
- Current-key-missing block (`currentKeyMissing && keyInfo`,
  `space-y-2 px-4 pb-2`): muted `e2ee.currentKeyMissing` (`text-xs`) + outline
  fullWidth `e2ee.requestCurrentKey` button (disabled while
  `loading || actionsPending || !canRequestKey`); after a successful request
  the `e2ee.currentKeyRequested` status line (`role=status text-xs
  text-text-muted`) appears below the button. `currentKeyMissing` = key known,
  inventory loaded, and no local row for it
  (`encrypted-chat-info-connected.ui.tsx:344-349`); `canRequestKey` = stream
  encrypted + `getRights().canRequest` (connected ui:350-352).
- Collapsible row: `PanelMenuRow label={e2ee.localKeysTitle(count)}` with
  chevron-up/down 20 in `text-text-muted` (`panel-menu-row.ui.tsx`: `py-1.5
  px-4` around a `min-h-8 gap-2` row -> 44 px total). Collapsed by default
  (`useState(false)`, ui.tsx:181).
- Rows (`LocalKeyRows`): `button` row `flex w-full items-center gap-2 px-4 py-2
  text-left hover:bg-sidebar-hover`, aria-label `e2ee.localKeyTitle + ": " +
  uuid`, chevron-right 16 `text-text-muted`. Inside `min-w-0 flex-1`:
  first line `flex items-center gap-2 text-xs leading-4` with
  `truncate font-mono` uuid rendered `first8...last4` (U+2026 between) and a
  badge `e2ee.localKeyCurrent` (`text-accent`) or `e2ee.localKeyHistorical`
  (`text-text-muted`); second line `mt-0.5 text-xs leading-4 text-text-muted`
  = `e2ee.localKeyAdded(date)`.
- Loading: `role=status px-4 text-xs text-text-muted` = `e2ee.loading`.
- Error: `role=alert text-xs text-danger` + sm neutral Retry button; the error
  text is `e2ee.localKeysLoadFailed` (connected ui:337 sets it from the
  inventory error).
- Empty: `px-4 text-xs text-text-muted` = `e2ee.noLocalKeys`.

Local key details screen (`encrypted-chat-info.ui.tsx:231-301`,
`section space-y-5 px-4`, aria-label `e2ee.localKeyTitle`):

- `e2ee.localKeyUuid` label `text-sm text-text-muted`; full UUID chip
  `select-text break-all rounded-lg bg-card-bg px-2 py-2 font-mono text-sm`;
  current/historical line `text-xs` in `text-accent` vs `text-text-muted`.
- `dl className="space-y-3 text-sm"`: `dt text-text-muted` ->
  `e2ee.localKeyAnnouncedLabel`/`e2ee.localKeyAddedLabel`, `dd mt-1` -> the
  labels.
- Outline fullWidth `e2ee.requestLocalKey` button (same disabled gate as the
  request-current-key button); after success the `e2ee.localKeyRequested`
  status line appears (`role=status text-xs text-text-muted`).
- Danger ghost fullWidth `e2ee.deleteLocalKey` button. The reference routes it
  through a shared confirmation dialog
  (`encrypted-chat-info-connected.ui.tsx:123-146`); see Deviations.
- Key vanished from the inventory: `role=status px-4 text-sm text-text-muted`
  = `e2ee.localKeyUnavailable`; loading/error reuse `e2ee.loading` /
  `e2ee.localKeysLoadFailed` + Retry at `text-sm`.

View shape (`encrypted-chat-info.types.ts:11-16`): `keyUuid`, `isCurrent`,
`addedLabel`, `announcedLabel`. Inventory rows come from the crypto key store
(`listStoredKeys` metadata `keyUuid` + `receivedAt`,
connected ui:100-115), sorted current-first (`Number(isCurrent)` descending,
stable), with `addedLabel = formatLocalKeyDate(receivedAt)` or
`e2ee.localKeyDateUnknown`, `announcedLabel = formatDateJoined(earliest
key_announced date)` or `e2ee.localKeyAnnouncementUnknown`.

Actions/failures (`chat-crypto-session.hook.ts`): requestKey failure toast
`e2ee.requestKeyFailed`; reissue failure `e2ee.rotateKeyFailed`; removeLocalKey
failure `e2ee.deleteLocalKeyFailed`. Reissue (`chat-crypto-runtime.lib.ts:829`)
stages a freshly created group key and posts a `key_announced` payload with the
rotation marker against `expectedCurrentKey = previous key`.

i18n keys (both locales, `packages/web/src/i18n/locales/{en,ru}.json`):

| Key | en | ru |
| --- | --- | --- |
| `e2ee.keyTitle` | Encryption keys | Ключи шифрования |
| `e2ee.keyUuid` | Current key ID | ID текущего ключа |
| `e2ee.copyKey` | Copy key ID | Копировать ID ключа |
| `e2ee.keyUnavailable` | Key unavailable | Ключ недоступен |
| `e2ee.rotateKey` | Reissue key | Перевыпустить ключ |
| `e2ee.loading` | Loading... | Загрузка... |
| `e2ee.retry` | Retry | Повторить |
| `e2ee.noLocalKeys` | No saved keys for this chat on this device. | На этом устройстве нет сохранённых ключей этого чата. |
| `e2ee.localKeyTitle` | Saved key | Сохранённый ключ |
| `e2ee.localKeysTitle` | Keys on this device ({{count}}) | Ключи на этом устройстве ({{count}}) |
| `e2ee.localKeyCurrent` | Current | Актуальный |
| `e2ee.localKeyHistorical` | Historical | Исторический |
| `e2ee.localKeyAdded` | Added: {{date}} | Добавлен: {{date}} |
| `e2ee.localKeyUuid` | Key ID | ID ключа |
| `e2ee.localKeyAnnouncedLabel` | Announced in chat | Объявлен в чате |
| `e2ee.localKeyAddedLabel` | Added to this device | Добавлен на это устройство |
| `e2ee.localKeyDateUnknown` | Unknown | Неизвестно |
| `e2ee.localKeyAnnouncementUnknown` | Not found in loaded history | Нет в загруженной истории |
| `e2ee.localKeysLoadFailed` | Could not load keys from this device. | Не удалось загрузить ключи с этого устройства. |
| `e2ee.localKeyUnavailable` | This key is no longer saved on this device. | Этот ключ больше не сохранён на устройстве. |
| `e2ee.requestLocalKey` | Request this key again | Запросить этот ключ повторно |
| `e2ee.localKeyRequested` | Request sent. The saved key remains available. | Запрос отправлен. Сохранённый ключ остаётся доступным. |
| `e2ee.deleteLocalKey` | Delete from this device | Удалить с этого устройства |
| `e2ee.requestCurrentKey` | Request current key | Запросить актуальный ключ |
| `e2ee.currentKeyRequested` | Request sent. Waiting for the key. | Запрос отправлен. Ожидаем ключ. |
| `e2ee.currentKeyMissing` | The current key is missing from this device. | Актуального ключа нет на этом устройстве. |
| `e2ee.keyCreatedAt` | Created: {{date}} | Создан: {{date}} |
| `e2ee.copyFailed` | Could not copy the key ID. | Не удалось скопировать ID ключа. |
| `e2ee.requestKeyFailed` | Could not send the key request. Please try again. | Не удалось отправить запрос ключа. Попробуйте ещё раз. |
| `e2ee.rotateKeyFailed` | Could not reissue the encryption key. Please try again. | Не удалось сменить ключ шифрования. Попробуйте ещё раз. |
| `e2ee.deleteLocalKeyFailed` | Could not delete the key from this device. Please try again. | Не удалось удалить ключ с этого устройства. Попробуйте ещё раз. |

Tokens: the screens use only tokens the native layer already binds
(`card-bg` chip fill, `text-muted`, `accent`, `danger`, `border-subtle`,
`sidebar-hover` hover), see the token table in
`docs/NATIVE_DESKTOP_E2EE_CHAT_INFO.md`.

## Figma record

File `Q3BtN9qwfQV8rAsyBAgCy1`, section `Шифрованные чаты` `10879:26030`. The
file tree snapshot in `~/dev/reference_state/figma/tree.json` (2026-11-13)
lists the key-screen frame `11026:27456` `right menu (у администратора)`
299x984 @ (13039, 17573) - the same 299 px right-menu family already cited in
#243 (`11024:26406` info, `11026:27394` settings). While this issue was
prepared the per-node endpoint `GET /v1/nodes?ids=11026:27456` returned
`404 Not found` (both `:` and `-` id forms) even though the tree snapshot
contains the frame, so the sub-node geometry could not be re-fetched. The
record above (React source at the pinned sha, measured class-by-class) remains
the geometry oracle, exactly as in #243; Figma keeps the same panel family and
the native drawer keeps the accepted 315 px `w-panel-right` width.

## Native design

Controller surface, next to the existing key API
(`native-controller.h:193-196`):

- `refreshChatLocalKeys()` - enumerates the durable vault for the selected
  stream (the "Initial durable key vault" of `docs/NATIVE_DESKTOP_CRYPTO.md`,
  `NativeChatKeyVault::listKeys`) and publishes `chatKeyInventory`:
  `{state: "loading"|"ready"|"error", error, keys: [{keyUuid, isCurrent,
  addedAt, announcedAt}]}`. `isCurrent` compares against
  `stream.security.currentKey`; `announcedAt` is the earliest `key_announced`
  message date for that key UUID among LOADED messages (same rule as
  `encrypted-chat-info-selector.lib.ts:88-96`); current-first stable sort like
  the reference. `state == "loading"` only while the store is still opening
  (`encryption().storeOpening`); enumeration itself is synchronous over the
  vault, so there is no artificial async phase. Reveal-driven: the QML screen
  calls it when the key/local-key screens are visible, following the
  no-fetch-while-hidden convention (#197).
- `encryption()` additions: `keyAnnouncedAt` (earliest `key_announced` ISO date
  of the current key, "" when absent), `canRotate` (owned-native verified
  stream with `owner`/`administrator` role and a valid current key - the
  native counterpart of `getRights().canRotate`), `reissueBusy`,
  `actionError` (localized action-failure text: `rotateKeyFailed` /
  `deleteLocalKeyFailed`), and the existing requested-key projection
  (`state == "requested"` + `keyUuid`) drives the reference `requestedKeyUuid`
  status lines without a second counter.
- `reissueChatKey()` - mirrors `requestChatKey`'s lifecycle: gates on vault
  match + encrypted stream, runs `ensureChatAuthority` first (the async
  verifier the request path already uses), requires the viewer role to be
  `owner`/`administrator` (the same right the reference reads as `canRotate`),
  stages a freshly created group key (`NativeChatCrypto::createGroupKey`)
  under the new UUID in the vault, and POSTs the `key_announced` message
  (`{kind: "key_announced", key_uuid: <new>}`) into the authority default
  topic. `reissueBusy` covers the whole window (verifying + sending), failure
  lands in `actionError` as `e2ee.rotateKeyFailed`. The stream's
  `current_encryption_key_uuid` flips when the stream update lands through the
  normal realtime/refresh path - the controller does not fake it.
- `deleteChatLocalKey(keyUuid)` - `NativeChatKeyVault::remove` for the
  (stream, key) address, then re-enumerates the inventory; failures (bad UUID,
  vault unavailable, store error) land in `actionError` as
  `e2ee.deleteLocalKeyFailed`. Success drops the row via `stateChanged`.
- `requestChatKey(keyUuid)` (existing) serves both request buttons:
  `requestChatKey()` empty arg targets the current key (the
  request-current-key recovery path already exposes
  `canRequestCurrentKey`), `requestChatKey(uuid)` re-requests a historical key.
- `formatChatKeyDate(isoDate, localeName)` - shared joined-date formatter
  (`MMM d, yyyy` en / `d MMM yyyy г.` ru), identical to the joined-date rule
  `profileDetails` already ships and to `formatDateJoined`. Empty input returns
  "" and the QML falls back to `Unknown` / `Not found in loaded history`.

QML: `NativeEncryptedChatInfo.qml` replaces the two scaffolds with
`KeySummary`/`LocalKeys` (key screen) and `LocalKeyDetails` (local-key screen),
mounted on the existing drawer Flickable with the #243 content-height rule
(each screen sizes only itself). Navigation reuses the shell stack:
`Main.qml` routes `screenChange("key")` and the new
`localKeyOpen(keyUuid)` signal to `rightDrawer.openChatScreen({screen:
"key"|"local-key", keyUuid})`, and feeds `page.keyUuid` from
`rightDrawer.currentChatKeyUuid` (both already scaffolded in #243). The copy
button writes the UUID through the same TextArea-copy pattern the encrypted
read path already uses for safe copy; the native test asserts
`QGuiApplication::clipboard()` afterwards.

## Deviations from the reference (measured, deliberate)

1. Delete confirmation dialog (resolved in R2): the reference wraps
   `e2ee.deleteLocalKey` in the shared ConfirmationDialog
   (`deleteLocalKeyConfirmTitle/Description`). The native drawer scope has no
   shared confirmation widget yet, so the details screen declares a
   page-scoped `Dialog` (`encryptedChatDeleteLocalKeyConfirm`) with the
   reference title/description copy; the danger button opens it and only the
   confirm button calls `deleteChatLocalKey`. Deleting a local key may drop
   the device's only copy of a historical generation, so the dialog names the
   consequence (messages become unreadable on this device; chat and other
   devices keep the key).
2. Inventory `loading` state only covers "store opening" (`storeOpening`):
   native vault enumeration is a synchronous durable read, so there is no
   fetch-in-flight phase to render; the reference's `loading` flag is its async
   storage read. Error and empty states port 1:1.
3. Reissue stages durably (resolved in R2): the reference stages creations
   crash-resumably (`listStagedCreations`/`writeStagedCreation`, recovered
   rotation) before announcing. The native action writes a `RotationStage`
   pin (vault record `rotation-stage-v1`) for the staged generation BEFORE
   the `key_announced` POST is dispatched. While the pin is set the controller
   refuses to delete that key, so a staged rotation key can never lose its
   bytes while the announcement outcome is unknown. The pin clears only on a
   verified announcement echo (the accepted `key_announced` message names the
   staged generation -> key becomes current) or a definite rejection (400 /
   401 / 403 / 404 on the announcement POST). Timeouts, 5xx answers and
   absence of the message are never treated as terminal; a success that
   landed while the process was gone is recovered by the GET recheck pass
   (`maybeResolveRotationStages`), armed on vault attach, login and inventory
   reveal. The pending/verifying/sending presentation and the failure text
   match.
4. No `KeyRequestHistory` section on the key screen (the request/grant flow is
   #222/#226 territory, out of this issue's acceptance list).
5. Drawer title: the reference title plumbing for the key screens is the shell
   `title` from the owning controller; the native shell keeps the chat title
   for both screens (consistent with #243's settings screen), the body-level
   section labels carry `e2ee.keyTitle`/`e2ee.localKeyTitle` semantics as
   accessible names.
6. Key-screen entry: the existing "Encryption keys" row in the settings screen
   (#243) remains the only entry; the reference's extra ordinary-drawer row for
   `sourceName === "native"` streams stays out of scope (same deviation as
   #243 deviation 3).

7. "Request this key again" on a locally-saved generation (resolved in R2):
   the reference runtime (`chat-e2ee-controller.lib.ts:185-230`) creates and
   posts a fresh `key_request` even while the local store holds the key,
   because staging leaves the local copy intact ("Request sent. The saved key
   remains available."). The details button now passes its explicit
   generation to `requestChatKey(keyUuid)`, and `startChatRequest` routes an
   explicit fresh request to the wire even when the vault row already holds
   the bytes: the request is staged as a pending request (never an empty
   bytes slot overwrite), the `key_request` POST goes out, and the sent
   status surfaces while the saved generation's bytes stay untouched
   (`chatLocalKeyRequestSavedGenerationSendsAndKeepsTheSavedBytes`). The
   implicit current-key recovery and the check/retry passes keep the original
   local short-circuit (a usable local generation is already the success
   state for those passes).

## Tests (issue acceptance)

Controller group `native-native-controller` (`native-controller-test.cpp`):

1. `chatLocalKeyInventoryEnumeratesVaultWithAnnouncementLabels`: seeded vault
   rows (current + historical key), a `key_announced` history row for each
   generation, and the projection pins UUIDs, `isCurrent`, current-first sort,
   `addedAt`/`announcedAt` ISO values and that the joined-date formatter
   matches en/ru joined styles; unknown announcement -> "".
2. `chatLocalKeyInventoryErrorAndMissingVault`: enumeration while the vault
   lease is stale reports `state == "error"` with the `localKeysLoadFailed`
   text; a refresh after the store comes back returns `ready` (retry).
3. `chatLocalKeyReissueAnnouncesAndFails`: reissue (owner role) shows pending
   via `reissueBusy`, posts exactly one `key_announced` message with the new
   UUID (mock-server assertion, no sleeps - deferred reply), stores the new key
   in the vault so the inventory grows; the deferred-failure variant surfaces
   the localized `rotateKeyFailed` text and clears pending; a member role is
   not allowed to reissue (no POST).
4. `chatLocalKeyDeleteRemovesAndFails`: delete removes the row from the vault
   and the inventory; a non-UUID argument and a stale vault report the
   localized `deleteLocalKeyFailed` text; the current key row stays deletable
   like the reference (the request-current-key recovery then applies).
5. `chatLocalKeyRotationAnnouncementPinsTheStagedKey`: while a rotation
   announcement is in flight (deferred POST) the staged generation is pinned:
   delete is refused with the localized pin text and the vault bytes stay
   intact; the pinned answer matrix — echo (201) releases the pin and delete,
   403 rejection clears it, 500 keeps it set with the row still pinned — and
   the success path stores the new key in the inventory.
6. `chatLocalKeyRotationPinSurvivesRestartAndDelayedEchoLands`: a new
   controller over the same vault directory sees the pin after restart
   (durable `rotation-stage-v1`), delete stays refused, and the delayed
   announcement echo landing (or the GET recheck seeing it on the server)
   resolves the pin so the row can be deleted afterwards.
7. `chatLocalKeyRequestSavedGenerationSendsAndKeepsTheSavedBytes`: requesting
   a generation the vault already saves posts a fresh `key_request`, reports
   the sent status, and leaves the local bytes (and revision) unchanged.

Window group `native-native-e2ee-ui` (`native-e2ee-read-test.cpp`, added to the
existing CMake function list - no new ctest group), data-driven en/ru x
dark/light over `ProtectedChatInfoScene`:

8. `uiEncryptedChatKeyScreenInventoryAndDetails`: the key screen shows the
   `Current key ID` chip (chip text = the stream key), the copy button puts the
   UUID on the clipboard, and the saved-keys section starts collapsed, then
   expands to Current/Historical rows (`first8...last4` monospace, added
   dates). A row opens the `local-key` details (full uuid, state line, the
   `Not found in loaded history` announcement fallback, added label); Back
   returns to the key screen. `Request this key again` mirrors the reference
   enable-gate and, for a generation the vault already holds, posts a fresh
   `key_request` and shows the sent-status line while the saved row stays
   intact (deviation 7); the current-key-missing recovery (delete the current
   generation's local row -> `Request current key`) exercises the same
   primitive with the bytes absent. The catalog snapshot that follows stream
   load keeps `canRequest` false until it settles; the case waits for the
   rights projection (same gate the reference reads from `getRights()`).
9. `uiEncryptedChatKeyScreenSourceGeometry` (data-driven en/ru x dark/light):
   the measured reference geometry - `text-sm` muted label over the card-bg
   chip (height 32), the 24 px ghost copy button, the 36 px `Reissue key`
   button, the 44 px collapsible row, the 50 px key rows with accent/muted
   Current/Historical badges, and the details labels/buttons - asserted
   against the native token palette (NativeTheme `card-bg`, `accent`).
10. `uiEncryptedChatKeyScreenDeleteConfirmation` (data-driven en/ru): pressing
   `Delete from this device` opens `encryptedChatDeleteLocalKeyConfirm` with
   the localized title/description naming the irreversible local-copy loss;
   Cancel closes it and the vault row (bytes included) stays intact; the
   confirm button removes the row and the details screen falls back to the
   unavailable state.

Every new `qsTr` string (context `NativeEncryptedChatInfo`) and every new
controller `tr` string lands in `packages/native/translations/workspace_ru.ts`
(native-native-locale enforces placeholder parity).
