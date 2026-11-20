# Encrypted chat header: stream-colored lock avatar and full protected-chat tooltip

Issue #90. Reference oracle: the read-only `workspace_ui` checkout at
`61dacef78dab2f5d0be76f283bebf49fe29b84af` (all `file:line` citations below are
that sha). Design fidelity case: parity-preserving - fix two measured
deviations, no redesign.

## Figma record

Figma token unavailable - mockups not consulted (`figma-token` returned a token
the API rejected with HTTP 403 `Token expired` on 2026-11-20 while reading file
`Q3BtN9qwfQV8rAsyBAgCy1`). The React source remains the oracle, as in
`NATIVE_DESKTOP_CHAT_HEADER_COUNTERS.md` and `NATIVE_DESKTOP_E2EE_CHAT_INFO.md`.

## Delta 1 - lock avatar background takes the stream color

Reference behavior:

- The encrypted header prepends `Avatar size="sm"` with
  `style={{ backgroundColor: resolveAvatarBackgroundColor(color) }}`
  (`packages/web/src/widgets/chat-view/chat-header-channel.ui.tsx:56`) and the
  `chatLock` icon `size={20}` `text-text-primary`
  (`chat-header-channel.ui.tsx:57`).
- `resolveAvatarBackgroundColor` (`packages/web/src/shared/lib/avatar-color.lib.ts:2-7`):
  an integer `0..0xffffff` renders as `#rrggbb` zero-padded to 6 hex digits
  (`avatar-color.lib.ts:7`), so `0` renders `#000000`; `null`, a non-integer or
  an out-of-range value returns `undefined` (`avatar-color.lib.ts:3-5`) and the
  Avatar keeps its theme fallback surface `bg-bg` plus a 1px `border-border-subtle`
  (`packages/web/src/shared/ui/avatar/avatar.tsx:61`). Avatar `sm` is
  `w-8 h-8` = 32x32 (`avatar.tsx:8`).
- Contract asserted in
  `packages/web/src/widgets/chat-view/chat-header-channel.ui.test.tsx:8-17`:
  `0x2563eb -> #2563eb`, `0 -> #000000`, `null -> bg-bg class` (and not `#000000`).

Measured native deviation: the header lock circle binds the flat `bg` token
unconditionally (`packages/native/qml/Main.qml:5682-5686`: `implicitWidth/Height: 32`,
`radius: 16`, `color: root.colors.bg`, `border.width: 1`,
`border.color: root.colors["border-subtle"]`). The 32/16/1px geometry already
matches the reference `Avatar sm` + `border-border-subtle`; only the background
binding deviates.

Fix (this change): bind the circle background to the selected stream's resolved
color hex, falling back to `root.colors.bg`. The resolution reuses the
already-reviewed native equivalent of `resolveAvatarBackgroundColor`:
`native-controller.cpp:12996-12997` projects each `streams()` row's
`avatarColor` as `"#%1".arg(*stream.color, 6, 16, '0')` when the stream carries
a color and as an empty string otherwise (`0` stays `#000000`), fed by the DTO
admission in `domain.cpp:492-493` which accepts only integer `0..0xffffff`.
Archived rows expose the raw integer row `color` (`native-controller.cpp:12907`);
the QML helper hexes it with the same padding rule. The lookup is a pure QML
binding over the existing `controller.streams` / `controller.archivedChats`
projections (same pattern as the sidebar avatar, `Main.qml:4986-4988`, and the
header-name lookup `root.encryptedStreamName`, `Main.qml:262`): no new fetch,
no new NOTIFY, nothing wired to the shared `stateChanged` signal. The stream
color flows through the existing `stream.updated` real-time path
(`native-controller.cpp:11805-11806` re-parses the full DTO, color included).
The 1px `border-subtle` stays painted in every case, matching the reference base
class where the inline `backgroundColor` overrides only `bg-bg`.

`color 0` semantics pin: `resolveAvatarBackgroundColor(0)` is `#000000`, never
the fallback - the QML predicate must test for the empty string produced by the
C++ projection, not for falsiness of `0`.

## Delta 2 - protected-chat tooltip shows title + five bullets

Reference behavior:

- The `AppTooltip` on the protected label (inside the `h1`,
  `chat-header-channel.ui.tsx:86-110`) carries a `space-y-2` block
  (8 px vertical rhythm): a title paragraph `font-medium text-text-primary`
  with `t("e2ee.chatFeatures")` (`:91`) and a `list-disc space-y-2 pl-4`
  (16 px marker indent) list of exactly five items in this order (`:93-97`):
  `chatFeaturesMessages`, `chatFeaturesKeys`, `chatFeaturesMissingKey`,
  `chatFeaturesMedia`, `chatFeaturesMediaAccess`.
- En texts (`packages/web/src/i18n/locales/en.json:1631-1637`):
  `protectedChat` "Protected chat"; `chatFeatures` "Protected chat
  information"; `chatFeaturesMessages` "Message text and all links within it
  are protected by end-to-end encryption."; `chatFeaturesKeys` "Keys are
  stored on the device. Different periods of chat history may require
  different keys."; `chatFeaturesMissingKey` "Without the matching key, a
  message cannot be read even if it has already been downloaded to the
  device."; `chatFeaturesMedia` "Images, videos, and files are stored without
  end-to-end encryption and can be accessed by the server and storage.";
  `chatFeaturesMediaAccess` "To open an attachment from the chat, you need to
  decrypt the message containing its link."
- Ru texts (`packages/web/src/i18n/locales/ru.json:1644-1650`): "Особенности
  защищённого чата" (title), "Текст сообщений и все ссылки внутри них защищены
  сквозным шифрованием.", "Ключи хранятся на устройстве. Для разных периодов
  истории могут понадобиться разные ключи.", "Без нужного ключа сообщение
  нельзя прочитать, даже если оно уже загружено на устройство.",
  "Изображения, видео и файлы хранятся без сквозного шифрования и доступны
  серверу и хранилищу.", "Чтобы открыть вложение из чата, нужно расшифровать
  сообщение со ссылкой на него."
- Surface (`packages/web/src/shared/ui/app-tooltip/app-tooltip.ui.tsx:208-215`):
  default size `w-80 px-4 py-3` (320 px wide, 16/12 px padding),
  `rounded-xl` (12), `border-border-subtle`, `bg-bg-elevated`, `text-sm
  leading-relaxed` (14 px, 1.625 line-height), body `text-text-secondary`.
  Trigger gap 8 px below the label (`app-tooltip.ui.tsx:23`, `:149`),
  align start (`chat-header-channel.ui.tsx:87`).
- Trigger: info icon 14 + `t("e2ee.protectedChat")` in the accent color,
  `bg-transparent p-0` (`chat-header-channel.ui.tsx:103-108`). Shown on hover
  and on focus (`app-tooltip.ui.tsx:97-107`); Escape closes
  (`app-tooltip.ui.tsx:160-166`). The tooltip wrapper is `relative z-10`
  (`chat-header-channel.ui.tsx:88`) above the title block's absolute
  right-panel toggle button (`chat-header-channel.ui.tsx:10-11`), so clicking
  the label must not toggle the right panel - asserted in
  `chat-header-channel.ui.test.tsx` (same test file, second case).

Measured native deviation: `protectedChatInformation` (`Main.qml:5733-5745`)
sets `ToolTip.text` to two of the five sentences joined by a newline
(`Main.qml:5744`) with no title and no list.

Fix (this change): replace the attached two-line text with a Controls `ToolTip`
popup whose content is a `ColumnLayout` with `spacing: 8` (`space-y-2`), a
title `Text` (`font.pixelSize: 14`, `Font.Medium`, `text-primary`, objectName
`protectedChatTooltipTitle`) and a `Repeater` over the five sentences, each a
list-item row with a disc marker inside the 16 px indent (`pl-4`), objectName
`protectedChatTooltipFeature-<index>`. Surface: width 320 (`w-80`),
padding 16 horizontal / 12 vertical (`px-4 py-3`), radius 12 (`rounded-xl`),
1px `border-subtle`, background `bg-elevated`, body 14 px / fixed 23 px line
height (`leading-relaxed`) in `text-secondary`. Visible on `hovered ||
visualFocus`, replacing today's identical visibility condition
(`Main.qml:5743`). The five en strings are the exact `en.json:1633-1637`
texts (two of them already ship in `workspace_ru.ts` with the exact
`ru.json:1648-1649` translations - "Keys are stored..." and "Without the
matching key..."); the three new strings get the exact `ru.json` texts in the
`Main` context of `workspace_ru.ts` so `native-native-locale` passes. The
title string "Protected chat information" already exists (the label's
`Accessible.name`, `Main.qml:5742`).

Deviations noted: the reference closes the tooltip on Escape and keeps a 120 ms
hover close-delay (`app-tooltip.ui.tsx:24,160-166`); native popups follow the
house pattern (immediate visibility bound to hover/focus state, e.g. the
reaction tooltips at `Main.qml:8525-8538`), and the popup offset uses Controls
popup placement rather than the reference 8 px start-aligned gap. Native never
wired the header title block to the right-panel toggle, so the z-order guard
has no native analogue; the test pins the behavior instead (a click on the
label leaves the right drawer state unchanged).

## States and tests

States: light + dark, en + ru, color set (`0x2563eb`) / `0` / absent.
Window tests live in `native-e2ee-read-test.cpp` (`native-native-e2ee-ui`
function list in `packages/native/CMakeLists.txt`), where the encrypted header
fixtures already render: `uiEncryptedSourceHeaderAvatarUsesStreamColor`
(32x32/r16 circle, border retained, `#2563eb` -> `#000000` via a real
`stream.updated` -> bg-token fallback when the DTO drops `color`) and
`uiEncryptedSourceProtectedChatTooltip` (hidden at rest; hover and keyboard
focus each open it; title + the five localized sentences as substrings;
exactly five `protectedChatTooltipFeature-*` list items; clicking the label
does not change the right drawer). The existing null-color test
`uiEncryptedSourceHeaderAvatarUsesDefaultThemeCircle` stays green untouched -
it is the `null -> bg token` half of the contract. No controller property,
fetch or signal was added, so the `bindingGets`/`streamGets` network-budget
tests in `native-native-e2ee-read` are unaffected; the suite was re-run as a
check. No test pinned the old two-line tooltip text (repo grep for both
sentences hits only `Main.qml` and `workspace_ru.ts`), so no stale expectations
required migration.
