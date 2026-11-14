# Encrypted chat info drawer: protected identity, info and settings screens (issue #243)

Part of #242. Reference contract measured at `workspace_ui` commit
`230e02b8b0273dee556a406746d122180a68b3e7`.

## Reference behaviour

Screen switch and shell (`features/encrypted-chat-info/encrypted-chat-info.ui.tsx:522-632`,
`encrypted-chat-info.types.ts:3`):

- The drawer shell owns all chrome; `EncryptedChatInfo` renders only the active
  screen body. `screen` is `"info" | "settings" | "key" | "local-key"`, the root
  carries `data-encrypted-chat-screen={screen}` and `aria-busy={loading}`, and
  `ScrollArea className="flex flex-1 flex-col gap-5 py-4"` gives a 20 px block
  gap with 16 px vertical padding. `loading` is a centered
  `Spinner size="sm"` + `e2ee.loading`; `unavailableReason` renders as a muted
  status line.
- Gating (`widgets/right-panel/right-panel-workspace-info.ui.tsx:828-845`): the
  encrypted presentation replaces the ordinary channel info when
  `stream.encryption === true`, or when `stream.sourceName === "native"` **and**
  the requested chat screen is `key`/`local-key`. A `sourceName === "native"`
  stream with `encryption !== true` keeps the ordinary info layer and only gains
  an extra "Encryption keys" row that pushes the `key` screen (:890-899).
  The projection itself (`encrypted-chat-info-selector.lib.ts:55-62`) returns no
  data for `encryption !== true && sourceName !== "native"`, and
  `encrypted-chat-info-connected.ui.tsx:321-329` feeds `title`, `color`,
  `description`, `participantsCount = members.length`, `onlineCount` and
  `channelTypeLabel = t(stream.isPrivate ? "channel.typeClosedProtected"
  : "channel.typeOpen")`.
- Chat screen routing (`widgets/right-panel/right-drawer.model.ts`, native port
  `qml/NativeRightDrawer.qml:160-186`): `openChatScreen({screen, keyUuid})`
  pushes onto the `chat-info` layer, dedupes the top screen, reuses an already
  listed screen instead of adding a Back loop, and Back pops only internal
  screens. Before this issue nothing consumed the chat-screen stack.

Info screen (`encrypted-chat-info.ui.tsx:562-627`):

| Block | Reference classes | Measured |
| --- | --- | --- |
| row | `flex items-center gap-4 px-4` | 16 px gap, 16 px side inset |
| avatar | `Avatar size="xl"` = `w-16 h-16 rounded-full bg-bg border border-border-subtle`, inline `backgroundColor: resolveAvatarBackgroundColor(color)`, `avatar.tsx:9` | 64x64, radius 32, 1 px `border-subtle` |
| lock glyph | `Icon name="chatLock" size={40}` in `text-text-primary`, `aria-label e2ee.protectedChat` | 40x40 glyph, centered (12 px inset) |
| name | `truncate text-xl font-medium leading-5` | 20 px / fixed 20 px line, Medium, elided |
| protected caption | `text-base leading-4 text-accent` | 16 px / 16 px, `accent` |
| counts | `text-sm leading-4 text-text-muted`, `t("channel.participants",{count}) + ", " + t("channel.online",{count})` | 14 px / 16 px, `text-muted` |
| text column | `flex flex-col gap-2` | 8 px between the three lines |
| action row | `flex gap-2 px-4`, each `Button className="h-12 flex-1"` | 48 px tall, two equal columns, 8 px gap, `rounded-lg` = radius 8 |
| Call | `variant="neutral"` = `bg-card-bg-active text-text-primary`, `Icon name="phone" size={32}`, `aria-label call.call` | 32 px icon |
| Settings | same base plus `bg-accent-soft`, `./assets/instant-mix.svg` 32 px, `aria-label settings.settings` | 32 px icon |

Settings screen (`ChannelSettings`, `encrypted-chat-info.ui.tsx:379-415`):

- `div className="space-y-5"` -> 20 px between blocks.
- `dl className="space-y-3 px-4"`: `dt text-sm text-text-muted` then
  `dd mt-1 text-sm text-text-primary` for `channel.channelName` -> `title` and
  `channel.description` -> trimmed `description`, falling back to
  `channel.noDescription` when empty (4 px label/value gap, 12 px between rows).
- `p className="px-4 text-xs text-text-muted"` -> `e2ee.channelSettingsReadOnly`
  (12 px).
- `PanelMenuRow label={t("e2ee.keyTitle")} icon="chatLock" iconSize={24}` with
  `right={<Icon name="chevron-right" size={20} className="text-text-muted"/>}`
  and `onClick: onScreenChange("key")`. `PanelMenuRow`
  (`shared/ui/panel-menu-row/panel-menu-row.ui.tsx`) is `py-1.5 px-4` around a
  `min-h-8 gap-2` row whose icon sits in a `h-8 w-8 text-text-secondary` box and
  whose label is `text-sm font-medium leading-5` -> 48 px row total.
- two `flex items-center justify-between gap-2 px-4 text-sm` rows:
  `channel.type` -> `channelTypeLabel` and `channel.members` ->
  `participantsCount`, value in `text-text-muted`.

i18n keys (both locales, `packages/web/src/i18n/locales/{en,ru}.json`):

| Key | en | ru |
| --- | --- | --- |
| `e2ee.protectedChat` | Protected chat | Защищённый чат |
| `e2ee.channelSettingsReadOnly` | Channel details are currently read-only. | Параметры канала пока доступны только для просмотра. |
| `e2ee.keyTitle` | Encryption keys | Ключи шифрования |
| `channel.channelName` | Channel name | Название канала |
| `channel.description` | Description (optional) | Описание |
| `channel.noDescription` | No description | Без описания |
| `channel.type` | Channel type | Тип канала |
| `channel.typeOpen` | Open | Открытый |
| `channel.typeClosedProtected` | Closed, protected history | Закрытый, защищённая история переписки |
| `channel.members` | Members | Участники |
| `channel.participants` | {{count}} participant / participants | {{count}} участник / участника / участников |
| `channel.online` | {{count}} online | {{count}} в сети (all forms) |
| `call.call` | Call | Позвонить |
| `settings.settings` | Settings | Настройки |

Tokens (`shared/lib/themes/blue-cold.ts`): `accent` `#7087ff` both modes,
`accent-soft` `#2b2d9a` dark / `#cde6ff` light, `card-bg-active` `#2c3747` dark
/ `#cde6ff` light, `text-muted` `#707070` dark / `#989898` light,
`text-secondary` `#999999` / `#787878`, `bg` `#141517` / `#e4ecf3`,
`border-subtle` `#33384d` / `#d8e4ef`. The native layer binds the existing
`root.colors[...]` tokens, which resolve the same palette per mode.

## Figma record

File `Q3BtN9qwfQV8rAsyBAgCy1`, desktop page, section `Шифрованные чаты`
`10879:26030` (Figma token from `figma-token`; the API returned
`429 Rate limit exceeded` while the last frame was being read, so the record
below is complete for the nodes cited and the React source remains the oracle).

- Info screen: `11024:26406` `right menu` 299x984 @ (10120, 17573), radius 6,
  column gap 20.
  - `11024:26407` text group 283x104 (header row 283x20 + identity row).
  - `11024:26415` identity row 283x64 @ (10128, 17633), gap 16.
  - `11024:26416` avatar circle 64x64, `cornerRadius: 32`; `11024:26417`
    `lock` group 40x40 @ (10148, 17645) -> 12 px inset; glyph vector 23.33x30.
  - `11024:26420` text column 187x64 @ x-offset 80 from the avatar left edge,
    gap 8: `11024:26421` name 16 px line, `11024:26422` `Защищенный чат`
    `#7d9ddf` (accent family), `11024:26423` `5 участников, 2 в сети`
    `#748396` (muted).
  - `11024:26424` action row 283x48 @ (10128, 17717), inner `button group` gap 8
    with two `137.5x48` tiles, `cornerRadius: 8`, icons 32x32
    (`11024:26427` call, `11024:26429` settings).
  - `11024:26432` member section (title 32 px row + 56 px rows) - deferred scope.
- Settings rows: `11026:27394` `right menu (у администратора)` 299x984
  @ (12713, 17573): label/value rows 267x32 with gap 12 - `11026:27422`
  "Ключ шифрования" row (32x32 icon box at x+8, 16 px label at x+52,
  16x16 chevron at the right inset), `11026:27432` `Тип канала` with the value
  `Открытый` in `#748396`, `11026:27443` members row with value `3`.
  `11026:27407` name/description blocks (label 20 px line + 32 px field).
- Key screen (out of scope, #244): `11026:27456` 299x984 @ (13039, 17573) with
  the current-key block and the request sections.

Figma uses the 299 px panel width and its own hex values; the native shell keeps
the accepted 315 px `w-panel-right` drawer (docs/NATIVE_DESKTOP_RIGHT_DRAWER.md)
and binds palette tokens instead of raw hex, so relative geometry (64/40/12,
16 and 8 gaps, 48 px action row, 32 px rows) is what transfers.

## Native design

**Component split (decision).** One new view,
`packages/native/qml/NativeEncryptedChatInfo.qml`, mounted on the existing
`NativeRightDrawer` body slot beside `NativeChannelInfo`. The reference is a
single component that switches on its `screen` prop, so the native port keeps
one component with a `chatScreen` property (`"info" | "settings" | "key" |
"local-key"`) instead of one file per screen: the info and settings screens
share the projection bindings, the same insets and the same scroll area, and the
1:1 mapping keeps future parity diffs readable. The drawer shell keeps all
chrome (title, back, close), exactly as the reference shell does, so
`NativeRightDrawer.qml` itself is unchanged. Navigation is exposed as
`signal screenChange(string screen)` - the counterpart of the reference
`onScreenChange` callback - and `Main.qml` routes it to
`rightDrawer.openChatScreen(...)`; the view never reaches into the shell.

- Gate: `Main.qml` shows the protected layer instead of `channelInfoLayer` while
  the top layer is `chat-info` with no profile override and
  `root.encryptedConversation` is true (authenticated + conversation selected +
  selected stream `encryption === true`, the existing `selectedStreamIsEncrypted`
  projection). That is the reference info-screen gate
  (`right-panel-workspace-info.ui.tsx:829, 843`).
- Screen projection: `rightDrawer.currentChatScreen` is `""` on the layer root,
  which maps to `"info"`; `"settings"`, `"key"` and `"local-key"` map to
  themselves. `Main.qml` feeds the property, the view renders the body.
- `key` and `local-key` render an empty scaffold (`objectName:
  encryptedChatKeyScreen`, `encryptedChatLocalKeyScreen`) so the navigation
  entry from the settings row lands on a real screen; #244 owns their content.
- Data: the same projection `NativeChannelInfo` already consumes -
  `controller.channelInfoStream` (title, description, `#rrggbb` color),
  `controller.channelInfoMemberCount`, `controller.channelInfoOnlineCount` -
  and the view triggers `controller.refreshChannelInfo()` on reveal, the
  established no-fetch-while-hidden convention (#197).
  `channelInfoStream()` gains `private` and `sourceName` so the type row can
  mirror `channelTypeLabel` and the projection can see the stream source
  witness; `privateStream` is the same API `private` field the reference reads
  as `isPrivate`.
- Call action: the reference passes the chat call bridge as `onCall`; the native
  counterpart is the existing `controller.canStartCall` /
  `controller.startCall()` pair already used by the conversation header.
- Back: the shell already shows Back whenever the top layer has a deeper screen,
  so Settings -> info needs no new affordance.

## Deviations from the reference (measured, deliberate)

1. The info screen renders the identity block and the Call/Settings row only.
   `KeySummary`, the key-request sections and `MemberList` stay out of scope
   (key management and requests are separate issues, member management already
   lives in `NativeChannelInfo`), and the `showKeyManagement` branch is not
   ported yet.
2. No `loading` / `unavailableReason` status line: the native projection is
   synchronous over already-fetched stream and binding rows, there is no
   `aria-busy` contract in the QML layer, and the counts render as soon as the
   binding response lands. `e2ee.loading` is therefore not added to the catalog.
3. The `sourceName === "native"` half of the reference gate is not implemented as
   an extra trigger for the `key` screen, because the only native entry to the
   key screen is the "Encryption keys" row inside the protected settings screen
   - the reference clause exists for the extra row that the *ordinary* info
   drawer shows for native-source streams, which this issue does not touch.
4. The reference "Encryption keys" label comes from `e2ee.keyTitle`
   ("Encryption keys" / "Ключи шифрования"); Figma shows the older
   "Ключ шифрования" wording. The React source at the pinned sha is the oracle.
5. Avatar background falls back to `colors["bg"]` when the stream carries no
   color (reference `Avatar` base `bg-bg`), matching
   `NativeChannelInfo.channelInfoAvatar`.

## Tests (issue acceptance)

Window group `native-native-e2ee-ui` (`native-e2ee-ui-test` binary,
`tests/native-e2ee-read-test.cpp`, `WORKSPACE_NATIVE_E2EE_UI`), data-driven over
`en`/`ru` x `dark`/`light` like the existing encrypted visual cases:

1. `uiEncryptedChatInfoDrawerShowsProtectedIdentity`: opening the chat-info
   layer on an `encryption=true` stream keeps `channelInfoLayer` hidden and
   shows the protected layer - 64x64 radius-32 avatar holding the 40 px
   `chat_lock.svg` glyph, the stream title, the accent "Protected chat" caption
   (en + ru catalog texts) and the participants/online line for the fixture's
   two bindings - and the ordinary header texts (`channelInfoTitle`,
   `channelInfoSubline`) are not rendered anywhere in the drawer.
2. `uiEncryptedChatInfoSettingsScreenNavigation`: the 48 px action row has two
   equal-width Call/Settings tiles; activating Settings switches to the settings
   screen with the read-only name/description list (description value, and
   "No description" fallback for a stream without one), the read-only hint, the
   "Encryption keys" row and the type + members rows; activating that row lands
   on the scaffolded key screen; the shell Back returns to the info screen.
   Geometry assertions cover the 16 px side inset, 8 px gaps, the 64/40 avatar
   inset and the 48 px action row.
3. `uiEncryptedChatInfoGateKeepsOrdinaryInfoForPlainStreams`: the same fixture
   with `encryption=false` keeps the ordinary `channelInfoLayer` visible and the
   protected layer absent, proving the ordinary drawer is untouched.
4. `native-native-locale`: every new `qsTr` string in the
   `NativeEncryptedChatInfo` context is reviewed in
   `packages/native/translations/workspace_ru.ts` (ru texts asserted in 1-2,
   en texts come from the same catalog keys' sources).
