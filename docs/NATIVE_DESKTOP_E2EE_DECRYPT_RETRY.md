# Per-message decrypt Retry on the encrypted message error row (issue #221)

Reference contract measured at `workspace_ui` commit
`230e02b8b0273dee556a406746d122180a68b3e7`.

## Reference behaviour

- `packages/web/src/widgets/workspace-message-list/workspace-message-content-bubble.ui.tsx:49-55`
  renders the error placeholder action when the presentation status is `error`:
  label `t("e2ee.messageDecryptRetry")`, `onClick: content.retry`,
  `disabled: content.retryPending`.
- `packages/web/src/features/encrypted-chat-message/encrypted-message-placeholder.ui.tsx:28-48`
  is the error row: `flex min-h-9` (min-height 36 px) `flex-wrap items-center
  gap-x-2` (8 px) `gap-y-1` (4px)` text-sm` (14 px font / 20px line)
  `text-text-muted`, with `Icon name="warning" size={16}` and the Retry as a
  `Button appearance="ghost" variant="neutral" size="sm"` with
  `!h-auto !rounded-sm !p-0 underline underline-offset-2`. Disabled buttons
  carry `disabled:opacity-50` (`packages/web/src/shared/ui/button/button.tsx:6`).
- `packages/web/src/features/message-presentation/message-presentation.hook.ts:153-165`
  re-queues only the clicked message (`phase: "queued"`, value kept), and
  `:263` defines `retryPending = allowed && entry?.value != null && entry.phase !== "settled"`:
  the row keeps showing the error text while the retry is in flight, with the
  button disabled. A success replaces the body; a repeated failure keeps the
  error row with a working Retry.
- Strings (both locales): `e2ee.messageDecryptError` / `e2ee.messageDecryptRetry`
  at `packages/web/src/i18n/locales/en.json:1769-1770` ("Could not decrypt
  message" / "Retry") and `ru.json:1782-1783` ("Не удалось расшифровать" /
  "Повторить"). `packages/native/translations/workspace_ru.ts` already carries
  both source strings, so no catalog change is needed.
- Palette: the row inherits `text-text-muted` (default theme
  `shared/lib/themes/blue-cold.ts:17,81`: light `#707070`, dark `#989898`).
  Native binds the existing `root.colors["text-muted"]` token, which resolves
  the same token per mode.

## Native gap

`packages/native/qml/Main.qml` `encrypted-error-*` row shows only the warning
icon plus "Could not decrypt message". All retries are key-level
(`retryChatKeyStore`, `retryChatKeyRequest`, `retryChatKeyResponse`).

## Native design

- `NativeController` gains `Q_INVOKABLE void retryMessageDecrypt(const QString
  &messageUuid)`. It is accepted only for the current encrypted conversation,
  only for an `e2ee` row whose current prepared entry exists, its payload hash
  matches, and its state is `Error` (i.e. exactly the state the error row
  renders). Anything else (locked store, unreadable conversation, missing key
  without an entry, already pending) is a no-op.
- A single accepted retry records the uuid in a per-message in-flight set,
  emits the dedicated `decryptRetryStateChanged` signal, and schedules one
  single-message re-prepare via `QTimer::singleShot(0, ...)`. The re-prepare
  re-runs the same per-row work as `prepareChatSlice` (vault key read, strict
  base64 envelope, `NativeChatCrypto::decryptMessage`, same `PreparedChatMessage`
  states) and then clears the flag and emits `decryptRetryStateChanged` again,
  so every in-flight transition is announced. `messagesChanged` publishes the
  refreshed entry.
- `Q_PROPERTY(QStringList decryptRetryPendingUuids READ ... NOTIFY
  decryptRetryStateChanged)` drives the QML disabled state. A getter over
  hidden in-flight state would never notify; the property's NOTIFY fires on
  every insert and erase (regression-tested with `QSignalSpy`).
- A second `retryMessageDecrypt` for an in-flight uuid returns before
  scheduling, so a double activation cannot double-decrypt one message.
- `qml/Main.qml` adds a ghost underline Retry inside the `encrypted-error-*`
  row, styled like the existing `InlineKeyRequestAction` ghost link
  (`Main.qml:303-327`): `NativeButton`, zero padding, `font.pixelSize: 14`,
  underline, fixed 20 px line height, `text-muted` color, opacity 0.5 when
  disabled (reference `disabled:opacity-50`), objectName
  `encrypted-retry-<uuid>`, `qsTr("Retry")`, click calls
  `controller.retryMessageDecrypt`.

## Deviations from the reference (measured, deliberate)

1. The issue cites `widgets/workspace-message-list/encrypted-message-placeholder.ui.tsx:27-33`;
   at the measured sha the file is
   `features/encrypted-chat-message/encrypted-message-placeholder.ui.tsx`
   (error row at :28-48). Geometry above comes from the real file at the sha.
2. QML `Text` has no `underline-offset`; the underline follows the existing
   native ghost-link convention (plain `font.underline`), matching every other
   inline text action in `Main.qml`.
3. The reference error row is a wrapping flex row. Native keeps the current
   `Row` (icon, error text, retry), letting the error text shrink; the 36 px
   minimum height and 8 px gaps match the reference measurements.
4. "Exactly one re-decrypt request" cannot be a network request in native:
   decryption is local (vault key + stored ciphertext). The vault already
   re-queues the whole window when key material changes
   (`NativeController::useChatKeys` observer, `keyMaterialChanged`). The
   per-message Retry covers the case where inputs did not change: the single
   re-decrypt cycle is proven by exactly one flagged pair
   (`decryptRetryStateChanged` add + clear) per accepted click, and repeated
   activations while in flight are dropped. Success replacing the body is
   proven end-to-end on the same single-message prepare path with a repaired
   key: the error row is replaced by the decrypted body.

## Tests (issue acceptance)

- `native-native-e2ee-read` (controller, `native-e2ee-read-test.cpp`):
  error row transition machine — retry sets the pending uuid and emits, a
  second in-flight activation adds nothing, completion clears and emits again
  and the row re-errors with the same bad key; retry while the store is locked
  (closed vault) is a no-op; retry of a key-missing row lands in `missing`
  with the flag cleared; `QSignalSpy` on `decryptRetryStateChanged` proves the
  property NOTIFY per transition.
- `native-native-e2ee-ui` (window, same source, `WORKSPACE_NATIVE_E2EE_UI`):
  mock a decrypt failure (known 32-byte key that cannot open the history
  envelope), assert `encrypted-retry-<uuid>` exists and is enabled, click it
  and assert the item is disabled and exactly one cycle is reported, the row
  stays an error with a working Retry, and after the key is repaired the row
  body is replaced by the decrypted plaintext.
