# Native desktop replies and quotes with jump to origin (release 0.9.0)

Issue: #6

Version: `0.9.0`. The latest published release is `0.8.0`; replies and quote
navigation add new functionality that preserves compatibility, so this is a
MINOR bump per the policy adopted in #92 (`Y += 1`, `Z = 0`).

Reference contract measured from `workspace_ui` at sha
`61dacef78dab2f5d0be76f283bebf49fe29b84af` (same pin as
`docs/NATIVE_DESKTOP_INLINE_EDIT.md` and `docs/NATIVE_DESKTOP_PARITY.md`).

## What the reference actually does

### Wire format (no new DTO fields)

A reply is a normal markdown message whose content starts with a quote
reference line. `features/workspace-reply/workspace-reply.lib.ts:190-212`
(`buildWorkspaceReplyMarkdown`) serializes each reply tab as
`[Name](urn:quote:<message-uuid>)` with an optional `?text=<selected text>`
query, then a blank line, then the answer text; trailing whitespace is
trimmed. The POST payload stays `{stream_uuid, topic_uuid,
payload:{kind:"markdown",content}}` — identical to a plain message.
The shared library `shared/lib/workspace-reference-urn.lib.ts` and the native
`src/workspace-reference.cpp:253-289` already parse and build exactly this
URN, and `buildWorkspaceQuoteReference(label, uuid, text)`
(`src/workspace-reference.h:58`) emits the exact source line format.

### Composer reply preview

Source: `widgets/message-composer/message-composer-reply-quote-preview.ui.tsx`,
`shared/ui/workspace-message-quote-frame/workspace-message-quote-frame.ui.tsx`,
`widgets/message-composer/message-composer-preface.ui.tsx:222-262`,
`shared/lib/chat-bottom-notice-bar.lib.ts:11,20-33`.

- Chrome sits above the composer input with `bg-composer-outer`
  (`#222328` dark / `#ffffff` light, `blue-cold.ts:32,94`), top corner
  radius 12 px, no bottom border.
- Quote row: horizontal inset 8 px, padding top 8 px / bottom 4 px,
  row gap 8 px.
- Quote frame (`surface="composer"`): radius 6 px, left bar 2 px accent
  `#7087ff` (`blue-cold.ts:18,81`), padding 8 px horizontal / 6 px vertical,
  fill same composer-outer color, vertical margin 0.
- Header: 12 px font / 16 px line height, weight 500, bottom margin 2 px,
  truncated, accent color. Text is `t("message.replyTo") + ": " + name` —
  `"Reply: Name"` / `"Ответить: <имя>"` (`message.replyTo` = `Reply` / `Ответить`).
- Body: 14 px / 20 px `text-text-primary` (`#ffffff` dark / `#1b1b1d`
  light), max two lines, text is the summarized quote content capped at
  `QUOTE_PREVIEW_MAX = 80` characters
  (`message-composer-constants.lib.ts:3`); no paragraph when empty.
- Cancel control: close icon 16 px with 4 px padding, muted color
  (`common.cancel` = `Cancel` / `Отмена`).

### Quote block inside a bubble

Source: `widgets/workspace-message-list/workspace-message-quote.ui.tsx`.

- Frame (`surface="message"`): fill `bg` at 35 % alpha (`#141517` dark /
  `#e4ecf3` light), radius 6 px, left bar 2 px accent, vertical margin 4 px,
  padding 8 px / 6 px (frame.ui.tsx:8-18).
- Header: 12 px / 16 px, weight 500, bottom margin 2 px, truncated.
  Normal color accent `#7087ff`; muted `#707070` dark / `#989898` light
  when the header is muted. Fallback label is the reference label, else
  `composer.quote` (`Quote` / `Цитата`) (quote.ui.tsx:53-62).
- Missing message (status `unavailable`): header becomes
  `message.quoteUnavailable` (`Message unavailable` / `Сообщение недоступно`),
  muted, and no body renders (quote.ui.tsx:113-115,434).
- Loading: header keeps the author label, body shows
  `chat.loadingMessages` (`Loading messages` / `Загрузка сообщений`)
  at 12 px muted (quote.ui.tsx:116-121).
- Selected-text branch: when the reference carries `?text=`, the body is the
  selected text rendered as plain wrapped text (quote.ui.tsx:144-151).
- Click-to-jump: the frame is a link (`message.openInChat` = `Open in chat` /
  `Открыть в чате`); click jumps to the origin message and ignores interactive
  targets and text selection (quote.ui.tsx:353-397).

### Quote resolution

`entities/messenger/messenger-quote-resolver.hook.ts:17-58`: a store hit is
`ready`; otherwise the app fetches `GET /messages/?uuid=` (durable cache
first, shared in-flight per uuid) via
`entities/messenger/messenger-quote-loader.lib.ts:253-288`; a miss is
`unavailable`. The native controller already performs the same resolve
request in `applyMessageRoute` (`src/native-controller.cpp:8621`,
`GET /messages/?project_id=&uuid=`).

### Jump to origin

`features/workspace-message-anchor-navigation/`: resolving an anchor uses
`GET /messages/<uuid>`/`?uuid=`, loads a window around it, and flashes the
target row for `2600 ms`. The native client already ships the equivalent
machinery from the message-links release
(`docs/NATIVE_DESKTOP_MESSAGE_LINKS.md`): `applyMessageRoute` resolves the
message, opens its topic, and `reanchorFromWindow` walks at most 8 older
pages, then highlights for 2500 ms (`native-controller.cpp:8604-8719`).
This release reuses that machinery instead of duplicating it.

### Reply entry and hotkey

The bubble context menu carries the reply action (`Reply` / `Ответить`,
`message.reply`). The reference additionally exposes a `Ctrl+L` quote hotkey
targeting the hovered or keyboard-focused row
(`workspace-message-list.ui.tsx:1286-1335`) and reply-with-selected-text.
See Deviations.

## Native design

### Reply session (controller)

`src/native-controller.h/.cpp` gains a reply session that mirrors the 0.8.0
edit-session pattern:

- `Q_PROPERTY QString replyMessageId NOTIFY replySessionChanged` — the quoted
  message uuid, empty when no session is open.
- `Q_PROPERTY QString replyAuthor NOTIFY replySessionChanged` — display name
  of the quoted author (`userDisplayName`).
- `Q_PROPERTY QString replyExcerpt NOTIFY replySessionChanged` — quoted
  content collapsed to one line and truncated at 80 characters with the
  `...` marker (`QUOTE_PREVIEW_MAX`).
- `Q_INVOKABLE bool beginReply(const QString &id)` — opens (or replaces) the
  session for a loaded message. Validates: message found, `kind ==
  "markdown"`, not an encrypted stream, authoritative current conversation
  (same guard as `editableMessage`). Fails with the notice
  `"This message can no longer be replied to."`. Starting a reply cancels an
  open edit session, and `beginEditMessage` closes an open reply session.
- `Q_INVOKABLE void cancelReply()` — closes the session.
- Route commits (`settleEditRouteChange` call sites) also close the reply
  session: the reply slot is scoped to the conversation that started it.
- `send()` composes the wire content with
  `workspace::buildWorkspaceQuoteReference(author, uuid) + "\n\n" + text`
  when a session is open; `SendIntent` carries the composed `content` and
  both POST acceptance and the `GET /messages/{uuid}` confirmation compare
  against the composed content instead of the raw draft. The composer keeps
  showing the answer as the draft; `canSend()` is unchanged (a blank answer
  cannot be sent). A confirmed send closes the session; a failed send keeps
  both the draft and the session (edit-session precedent).

The context menu entry `replyMessageAction` now calls `beginReply` instead of
the markdown-blockquote `quoteMessage` (removed together with its test).

### Quote references in message rows

`parseMessage` stays unchanged (the reference lives inside the markdown
content). The row map gains, for messages whose first non-blank line is a
standalone quote reference (regex over the trimmed content; the reference
label, uuid and optional `?text=` are parsed through
`workspace::parseWorkspaceReference`, the same parser the sender uses):

- `quoteId` — origin uuid (empty otherwise).
- `quoteLabel` — reference label (fallback author name).
- `quoteText` — decoded `?text=` selection, empty when absent.
- `displayContent` — content with the quote line removed (equal to `content`
  when there is no leading reference). The bubble body renders
  `displayContent`; `content` stays intact for edit prefill, copy and link
  actions.

Resolution state per `quoteId` is served from a controller-owned quote store
(consistent with "cache first, fetch on miss"):

1. A loaded message with that uuid in the current window resolves
   immediately: `quoteStatus = "ready"`, `quoteAuthorName` from
   `userDisplayName`, `quoteContent` = its raw markdown content.
2. Otherwise the controller issues `GET /messages/?project_id=&uuid=` once
   per uuid (shared in-flight, deduplicated). While pending,
   `quoteStatus = "loading"`. A parseable answer marks it `ready`; an empty
   or invalid answer marks it `unavailable`. When resolved, `messagesChanged`
   re-announces the rows.

`Q_INVOKABLE void openMessageInChat(const QString &id)` routes through the
existing `applyMessageRoute` org/project guard, so origins in not-yet-loaded
history reuse the bounded 8-page walk, the `messageHighlight-<uuid>` flash
and the "unavailable" notice unchanged.

### QML (`qml/Main.qml`)

- Reply notice `replyNotice`: clones the `editMessageNotice` strip pattern
  (33 px row rule, `composer-outer` fill, 1 px `border-subtle`) with the
  measured quote frame inside: accent bar `replyAccentBar` 2 px `#7087ff`,
  radius 6 px, padding 8/6, `replyAuthor` 12 px/16 px weight-500 accent
  text `"Reply: <name>"`, `replyExcerpt` 14 px/20 px two-line elide, and
  `cancelReplyButton` (`Cancel`, muted) on the right. Visible while
  `controller.replyMessageId` is non-empty.
- Keys: Escape cancels the reply session when it is open and no edit session
  is active (edit keeps priority).
- Bubble quote block `message-quote-<id>`: the first child of `bubbleContent`,
  drawn from the row map: accent bar + 35 % alpha fill + radius 6 px frame;
  header = `quoteAuthorName` (fallback `quoteLabel`, else `"Quote"`),
  body = `quoteText` as plain wrapped text when present, otherwise the
  rendered `quoteContent` markdown body; `loading` shows `"Loading
  messages"`, `unavailable` shows `"Message unavailable"` muted. Clicking
  (Enter, link-free area) calls `controller.openMessageInChat(quoteId)`.
- `openReference` also navigates `urn:quote`/`urn:message` anchors through
  `openMessageInChat` instead of the generic reference notice.

### i18n

New `Main`/`NativeController` strings added to
`packages/native/translations/workspace_ru.ts` (locale guard):
`Reply: %1` = `Ответить: %1`; `Quote` = `Цитата`; `Message unavailable` =
`Сообщение недоступно`; `Loading messages` = `Загрузка сообщений`;
`This message can no longer be replied to.` =
`Это сообщение больше нельзя открыть для ответа.` Existing entries are reused
for `Reply` and `Cancel`.

## Deviations from Source (named and justified)

- Single reply slot instead of multi-tab reply sessions with drag reorder
  (`features/workspace-reply` tabs): the Qt list/composer has no drag surface
  yet and one release cannot carry the tab UX. The wire format stays fully
  compatible (one tab is what a one-tab source session serializes).
  Follow-up issue filed.
- `Ctrl+L` quote hotkey and reply-with-selected-text are deferred: the source
  targets the hovered row or a DOM text selection, neither of which exists
  for Qt message rows today (no row focus model, no text selection in
  bubbles). Follow-up with the row-focus work.
- Anchor highlight stays 2500 ms (native message-links contract) versus the
  source `2600 ms` flash: reuses shipped, tested timing; no user-visible
  contract change.
- Nested quote rendering inside a resolved quote body is not recreated: the
  source recurses to depth 12 with a cycle guard; nested references inside a
  quote body render as plain links. Single-level quotes are the composer's
  own output shape.
- Editing a reply message prefills the raw canonical markdown (the reference
  line is visible text) instead of restoring the quote into the banner
  (source `workspace-reply-restore.lib.ts`). Follow-up.
- Cross-conversation reply handoff is not ported: leaving the conversation
  closes the reply session (mirrors the edit-session route guard rule
  shipped in 0.8.0).
- Replies in encrypted streams stay unavailable (menu entry hidden), matching
  the shipped e2ee message-action gate.

## Tests

- `native-native-controller`: reply session opens/replaces and exposes the
  80-char excerpt; a confirmed send posts and verifies the composed
  `urn:quote` content and closes the session; a failed send keeps draft and
  session; Escape-equivalent `cancelReply` and a route change close the
  session; a message with a leading quote reference yields row `quoteId`,
  `quoteLabel`, `quoteText`, `displayContent`; quote status goes
  `loading -> ready` via `GET /messages/?uuid=` for an unloaded origin and
  `-> unavailable` for a miss; `openMessageInChat` anchors a loaded message
  and deep-walks for an unloaded origin (existing bounded-walk path).
- `native-native-window`: `replyFlowUsesComposerBannerAndJump` — reply from
  a peer message shows `replyNotice` (33 px strip, 2 px accent bar,
  `Reply: Bob` header, excerpt body), Escape cancels, send keeps the
  `sendButton` objectName; an incoming message with a quote reference shows
  `message-quote-<id>` with the measured frame, clicking it sets
  `controller.anchorMessageId` and surfaces `messageHighlight-<origin>`.
- `native-native-locale`: catalog guard covers the new strings.
