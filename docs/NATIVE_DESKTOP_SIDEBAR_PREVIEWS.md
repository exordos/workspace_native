# Native desktop sidebar previews, timestamps and activity order (release 0.7.0)

Issue: #2 (P2-high) - "Everyday sidebar: last-message previews, timestamps and activity
ordering, using the existing cache and Source row geometry."

Version: `0.7.0`. The milestone label `0.5.0` is a provisional roadmap estimate. Per the
version policy (`NATIVE_DESKTOP_DEVELOPMENT.md#versioning-and-releases`, clarified in PR #92)
the next version is computed from the latest published release `0.6.0`; these are compatible
new features, so the bump level is MINOR.

Reference contract measured from `workspace_ui` at sha
`61dacef78dab2f5d0be76f283bebf49fe29b84af` (the revision pinned in
`NATIVE_DESKTOP_PARITY.md`; the reference tree has not moved since).

## Reference contract (measured)

### Preview line - `packages/web/src/widgets/sidebar/sidebar-message-preview.ui.tsx`

- One line: `min-h-5 min-w-0 truncate text-xs font-normal leading-5` - a 20 px line box
  (12 px font, 20 px line height), single line, ellipsis, and it is **always mounted**:
  the source comment says an empty box without `min-h-5` collapses to 0 height, which would
  make a topic row without a last message shorter than a filled one.
- Segments: sender in the `sidebar-sender` palette token, then `": "` when both sender and
  body are present, then the body in `text-text-secondary` (source comment: Figma preview
  body `~#9AA8D8`, i.e. secondary, not muted).
- `sidebar-sender` hex per palette (`shared/lib/themes/*.ts`): `blue-cold` `#7087ff`
  (light and dark), `blue-mist` `#8aa8e8` light / `#6f90d8` dark, `emerald-chat`
  `#63e09a` light / `#2bbf6c` dark, `orange-warm` `#ff8438`. The native theme already
  carries the same token values per palette (`qml/NativeTheme.qml`).

### Right meta column - `packages/web/src/widgets/sidebar/sidebar-chat-row-meta.ui.tsx`

- Column `flex shrink-0 flex-col items-end min-w-10 justify-between gap-1` in normal density:
  a 40 px column with the badge row on top and the timestamp at the bottom.
- Timestamp `whitespace-nowrap text-xs tabular-nums text-text-muted` (12 px, muted token,
  tabular figures) and only rendered when the row is not compact and the time string is not
  empty.

### Row geometry - `packages/web/src/widgets/sidebar/sidebar-chat-row-layout.lib.ts`

- Stream/DM row: `flex items-stretch gap-3 rounded-lg px-2.5 py-2.5` (gap 12 px, padding
  10 px, radius 8 px) with body `flex min-w-0 flex-1 flex-col justify-between`, i.e. title at
  the top of the column and preview at the bottom. Row height
  `10 + max(40 avatar, 20 + 20 text) + 10 = 60` px.
- Topic row: `flex items-stretch gap-3 rounded-lg py-2 pl-[38px] pr-2` with the 3 px accent
  bar (`w-[3px] shrink-0 self-stretch rounded-full`) as the leading flex column, title
  `truncate text-sm font-medium leading-5 text-text-primary` (14 px / 20 px). Row height
  `8 + (20 + 20) + 8 = 56` px. Topic list spacing `mt-0.5 space-y-0.5` (2 px).
- The native rows already reserve exactly these heights (`streamButton` 60 px,
  `topicButton` 56 px in `qml/Main.qml`), so the preview and the timestamp are placed inside
  the existing boxes: no row height changes.

### Preview content - `packages/web/src/entities/messenger/messenger-topic-list.lib.ts:61-92,156-188`

- `messageUuid == null` or a message the client has not hydrated -> no preview object at all;
  the row renders the empty 20 px line box. Previews are explicitly best effort: only
  messages the client currently holds participate.
- Sender: `common.you` ("You" / "Вы") for the current user, otherwise the resolved user
  display name; when the author is unknown the sender segment is omitted entirely.
- Text: markdown summary of the body with
  `DEFAULT_WORKSPACE_MESSAGE_SUMMARY_OPTIONS` = `maxLength: 120`, media and attachment labels
  enabled; `truncatePreview` collapses whitespace runs to one space, trims, and appends the
  `"..."` marker (never a Unicode ellipsis).
- Non-markdown payload -> empty text plus the `encrypted` marker, and
  `widgets/sidebar/sidebar-workspace-message-preview.ui.tsx` then renders the decrypted text
  when a key is available, otherwise `e2ee.lockedTitle` ("Key required" / "Нужен ключ"),
  `e2ee.messageDecrypting` ("Decrypting message..." / "Расшифровка сообщения...") or
  `e2ee.messageDecryptError` ("Could not decrypt message" / "Не удалось расшифровать").
- Derived preview texts sit in an LRU bounded by 512 entries and 1 MiB and are dropped when
  the session owner changes (same file, `previewTextCache`).

### Timestamp format - `packages/web/src/shared/lib/datetime.lib.ts:65-82`

`formatMessageTimeRelative(unixSeconds)`:

1. same local calendar day -> `HH:mm`, built manually (24 h, zero padded, not `Intl`);
2. previous local calendar day -> the `chat.yesterday` label only ("Yesterday" / "Вчера"),
   no clock time;
3. otherwise a compact locale date, `day: numeric, month: short` (`5 нояб.` in ru-RU,
   `Nov 5` in en-US).

This is a different function from the activity-list formatter already ported for mentions
(`formatActivityItemTime`, which appends a time after the yesterday label); the two must not
be merged.

### Ordering - `entities/messenger/messenger-sidebar.lib.ts:211-290,522` and
`entities/messenger/messenger-topic-list.lib.ts:108-149`

- Default sort mode is `last_message` (`selectMessengerSidebarStreams`, line 522).
- Streams (`compareSidebarStreams`): pinned first, then group rank (archived 2, fully muted 1,
  otherwise 0), then last-message `createdAt` descending with unknown activity last
  (`compareNullableStringsDesc`), then `orderIndex`. The activity source
  (`streamSortMessageCreatedAt`) falls back to the stream `createdAt` when the stream has no
  `lastMessageUuid` at all - a known-but-unhydrated last message does not fall back.
- Topics (`compareTopics`): group rank (resolved 2, effectively muted 1) then
  `lastMessageCreatedAt ?? updatedAt` descending.
- `useSidebarStreamOrder` additionally *holds* the previous activity instant for a stream
  whose topics were resolved during the visit, so completing a topic cannot push a stream
  down while the user is looking at the list. That hold is session-local presentation only.

## Native state today

- `NativeController::topics()` already implements the reference topic comparator: rank
  (resolved, then muted) then the last-message `createdMicros` with `updatedMicros` as the
  fallback, stable sorted. Only the preview and the timestamp are missing.
- `NativeController::streams()` returns server order with no activity comparator, and no
  native row shows a preview line or a row timestamp.
- Message bodies for conversations that are not currently open are reachable through the
  existing disk cache: `NativeCacheService::getMessageBody(lease, uuid, receiver, callback)`
  resolves an exact Body row or at most four indexed History pages, off the GUI thread.
- `Stream::lastMessage` / `Topic::lastMessage` hold the last-message UUID;
  `Stream::updatedMicros` / `Topic::updatedMicros` hold the row `updated_at`.

## Native design

### 1. Projection fields

`streams()` and `topics()` gain three display fields per row, leaving every existing key and
its meaning untouched:

- `previewSender` - display name of the last-message author, `common.you` for own messages,
  empty when the author is unknown.
- `previewText` - the one-line summary, already whitespace-normalised and clamped; empty when
  the message is not resolved.
- `previewLocked` - true when the row's last message is an E2EE payload the client cannot
  present; the QML layer then shows the `e2ee.lockedTitle` text, mirroring the reference
  encrypted branch.
- `rowTime` - the `formatMessageTimeRelative` string of the last message, empty when the
  message is not resolved.
- `activityMicros` - the resolved activity instant used by the comparator and tests (the
  reference `streamSortMessageCreatedAt` equivalent: last-message `createdAt`, else the row
  `updated_at`).

Summary construction reuses the existing native plain-text path (`NativeMarkdown::render(...)
["plain"]`), then applies the reference preview rules: collapse whitespace runs, trim, clamp
to 120 characters with the `"..."` marker. `mentionPreview()` keeps its own 80-character
activity contract and is not reused; the two formatters have different maxima in the reference.

### 2. Bounded, asynchronous hydration

Row projections must not touch SQLite synchronously: `streams()` and `topics()` are QML
bindings evaluated during polish. Instead:

- an in-memory `QHash<QString, RowPreview>` keyed by message UUID, filled from the messages
  the controller already holds (history pages, live events, own sends) and from cache reads;
- after any stream/topic/message change, a bounded pass resolves at most eight unresolved
  last-message UUIDs per tick through `NativeCacheService::getMessageBody`, one queued
  callback each, and drops the pass when the session guard or the cache lease changes;
- the map is bounded (512 rows, mirroring the reference preview cache) and is cleared
  together with the rest of the owner-scoped state on sign-out, so no row preview survives
  an owner change.

A row whose last message cannot be resolved keeps the empty line box, which is exactly the
reference behaviour for a message the client has not hydrated.

### 3. Ordering

`streams()` sorts by the reference comparator, adapted to the fields the native model has:

1. group rank: archived rows are already filtered out of `streams()`, so rank 1 is the fully
   muted stream (stream mode `muted`, or stream mode `default` with every topic effectively
   muted), rank 0 otherwise;
2. last-message activity descending using `activityMicros`, unknown activity last;
3. `std::stable_sort`, so ties keep the current server order (the reference tiebreak is
   `orderIndex`, which the native model does not receive).

Native has no pin field on the Workspace stream DTO, so the pinned segment of the reference
comparator is not portable yet; that is tracked by the sidebar-context-menu issue #98, which
is where pin/unpin arrives.

### 4. QML rows

- Stream row (`streamDelegate`): the title stays at the top of the 40 px text column and the
  preview line is added at the bottom (`Layout.alignment: Qt.AlignBottom`), 12 px font with
  `lineHeight: 20`, `elide: Text.ElideRight`. Sender uses `colors["sidebar-sender"]`, body
  `colors["text-secondary"]`. The unread badge column is replaced by the reference meta
  column: a 40 px wide column that keeps the badge on top and shows `rowTime` in 12 px
  `colors["text-muted"]` at the bottom.
- Topic row: same structure inside the existing 56 px box - title at `y: 8`, preview at
  `y: 28`, time in the same right column; the 3 px bar keeps its `x: 38` inset.
- Both rows are laid out so that an empty preview still occupies the 20 px box (the row
  height is fixed at 60/56 px anyway, which is the same invariant the reference enforces with
  `min-h-5`).

### 5. i18n

New visible strings (added to `packages/native/translations/workspace_ru.ts` because
`native-native-locale` guards the catalog): "You", "Yesterday", "Key required".
The en/ru pair matches the reference keys `common.you`, `chat.yesterday` and
`e2ee.lockedTitle`.

## Deviations and follow-ups

- No pin ordering (no native pin DTO), as above.
- The reference timestamp carries the `tabular-nums` font feature
  (`sidebar-chat-row-meta.ui.tsx`). QML exposes no font-feature API on the `Text.font`
  group, so the native row uses the default figure substitution; row times are static per
  bind, so no width jitter is observable. Recorded here rather than hacked around.
- The reference *stream order hold* while topics are being resolved during a visit
  (`useSidebarStreamOrder`) is not ported in this release: the native comparator reads the
  same resolved-topic state, so a resolved topic can move a stream down immediately. The
  reference hold is a presentation-only anti-flicker rule; porting it needs a per-list
  snapshot. Noted here rather than guessed at; if it proves visible in practice it becomes its
  own bug.
- Cache-backed hydration is bounded to eight rows per pass; a conversation that was never
  visited has no cached body and therefore no preview, exactly like the reference for a
  conversation it has not hydrated.

## Test plan

- `native-native-controller`: preview composition (sender, own message, clamped 120 characters
  with the `"..."` marker, whitespace collapse, encrypted row), relative time (today,
  yesterday, older, locale shape), and the stream comparator (activity descending, unknown
  activity last, muted group after active, stable ties).
- `native-native-window`: a stream row and a topic row render the preview line and the time at
  the measured geometry, and a row without a resolved message keeps the full row height.
- `native-native-locale`: catalog guard for the new strings.
