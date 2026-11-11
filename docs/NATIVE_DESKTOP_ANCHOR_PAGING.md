# Bidirectional boundary paging around a message anchor

Issue #71 (`Message anchor: page newer messages when the anchor sits
mid-history`). Design fidelity case: parity port of an existing Electron/React
behaviour, no redesign.

## Reference contract (workspace_ui 61dacef78dab2f5d0be76f283bebf49fe29b84af)

Measured from source at that revision:

- `packages/web/src/pages/chat/chat-pagination.lib.ts` -
  `deriveFocusedPaginationFlags(messages, focusedMessageId)`: without an
  anchor the flags are the regular chat behaviour `{hasOlderMessages: true,
  hasNewerMessages: false}`; with an anchor both flags are derived from the
  loaded ids relative to the anchor (an id below the anchor sets older, an id
  above it sets newer). `shouldLoadBoundaryPage` guards a boundary request:
  in flight, no boundary messages, or an empty window all skip.
- `packages/web/src/shared/api/messenger-messages.api.ts` -
  `getMessagePagesAroundResolvedMessage` (contract test
  `messenger-messages.api.test.ts:169-246`): two parallel pages around the
  anchor uuid, both with `page_limit`, `sort_key=created_at`,
  `stream_uuid` and (for topics) `topic_uuid`:
  - older page: `sort_dir=desc`, `page_marker=<anchor uuid>`; the marker for
    the next older page comes from the `X-Pagination-Marker` response header
    (`beforePageMarker`);
  - newer page: `sort_dir=asc`, `page_marker=<anchor uuid>`; the next newer
    marker likewise (`afterPageMarker`).
  The rows are ordered chronologically ascending: `before + anchor + after`.
- `packages/web/src/entities/messenger/messenger-messages-loader.lib.ts`
  (`resolveOpeningMessageWindow`, lines 838-931): the conversation first
  loads the regular tail page; if a resolved anchor is not in that page, the
  around-anchor window above REPLACES the window, carrying both boundary
  markers. `retainConversationWindow` (lines 776-807) keeps the existing
  window when a fresher tail snapshot is newer than the retained tail.
- `packages/web/src/widgets/workspace-message-list/workspace-message-list-scroll.hook.ts:1199-1208`:
  the reference auto-loads the newer page when the viewport reaches the bottom
  while `hasNewerMessages` is set; the older boundary mirrors it at the top.
- The anchor highlight is a dedicated overlay (`app.styles.css:158-170`,
  12 px radius) kept for 2.5 s; the native highlight timer already matches.

## Current native gap

`NativeController` only has an older boundary: `m_before` drives
`canLoadOlder` (`native-controller.h:226`) and `loadOlder()`. The deep-link
search (`applyMessageRoute` + `reanchorFromWindow`, issue #55) walks at most
8 older pages from the tail window to find the anchor and never pages newer,
so a resolved mid-history anchor cannot surface the rows below it and, when
the walk cannot reach the anchor, the conversation stays tail-only
(`docs/NATIVE_DESKTOP_MESSAGE_LINKS.md`).

## Native design

Same transport as the reference, ported into the existing controller guards
(`m_stream`/`m_topic`/`m_selection`/`m_messageRevision`/`m_cacheLease`
patterns already used by `historyPage`).

1. Newer boundary state: `QByteArray m_after` next to `m_before`.
   `Q_PROPERTY(bool canLoadNewer READ canLoadNewer NOTIFY stateChanged)`
   mirrors the `canLoadOlder` gating:
   `!m_after.isEmpty() && !m_loading && !m_snapshotActive &&
   (!m_cacheProtection || m_historyVerified)`. An empty marker means that end
   is reached, so both boundary flags flip from the server's own
   `X-Pagination-Marker` presence, like the reference. `m_after` is cleared
   wherever `m_before` is reset and whenever a tail (newest-first, no marker)
   reload commits, because a tail window has no newer side.
2. Around-anchor opening: when a deep-link anchor is resolved and not found in
   the loaded window (the point where `reanchorFromWindow` today starts the
   older walk), the controller issues the reference's two parallel requests
   once per anchor: `sort_dir=desc&page_marker=<anchor>` and
   `sort_dir=asc&page_marker=<anchor>` with `page_limit=50` (the reference's
   `DEFAULT_MESSAGES_PAGE_LIMIT`). On success the window becomes the
   chronologically ascending `before + anchor + after`, `m_before` and
   `m_after` take the two markers, and the normal commit sequence follows
   (`messagesChanged`/`topicsChanged`/`announceStreamRowPreviews`/
   `stateChanged`), which lets `reanchorFromWindow` find the anchor and start
   the 2.5 s highlight. Rows equal to the anchor or outside the conversation
   are skipped; an empty page marker on either side means that end is reached.
   The existing bounded 8-page older walk stays as the fallback for any
   around-window failure (non-2xx, wrong selection after landing), which also
   keeps every #55 test path intact.
3. `Q_INVOKABLE void loadNewer()`: guarded like `loadOlder()`, requests
   `sort_dir=asc` with `page_marker=m_after`, merges the page into
   `m_messages` (`mergeMessages` sorts chronologically ascending), advances
   `m_after` to the page's marker, and emits the usual change signals. It
   touches no anchor state, so an active highlight survives a newer page.
   A failed request reuses the existing `Could not load messages. %1` notice.
4. Around/newer pages bypass the on-disk history-page cache: the cache key is
   the linear older marker and neither new direction fits it; a cold cache
   simply refetches (documented limitation, no cache pollution).

QML: one button under the message list mirroring the established
**Load older messages** button above it (`Main.qml:3103-3110`): visible when
`controller.canLoadNewer`, text `qsTr("Load newer messages")`, enabled
without `controller.busy`, clicking `loadNewer()`. Deviation from the
reference is deliberate and precedented: the reference auto-loads boundary
pages from scroll edges; the native client exposes explicit boundary buttons
and adds no scroll-edge auto-load in either direction. The new source string
is added to `translations/workspace_ru.ts` ("Load newer messages" ->
"Загрузить более новые сообщения") to keep `native-native-locale` green.

## Acceptance (extends `native-native-controller`)

With the synthetic server, history sized so the anchor sits mid-history:

- opening the deep link resolves the anchor and the window shows rows newer
  than the anchor (the `sort_dir=asc` request carries
  `page_marker=<anchor uuid>`); `anchorMessageId` is set and highlights;
- `canLoadNewer` is true while the newer marker is non-empty; `loadNewer()`
  issues `GET ...sort_dir=asc&page_marker=<after marker>`, appends the page
  without clearing the anchor, and flips `canLoadNewer` to false when the
  server omits the next marker;
- `canLoadOlder` flips to false when the older side runs out (empty marker);
- default tail loads keep `canLoadNewer` false (no anchor => no newer side);
- a failing around-window request falls back to the existing bounded older
  walk and still anchors (or lands safely), so every #55 contract stays green.

Window-level geometry is unchanged: the new button reuses the existing
`NativeButton` style and layout slots, so no `native-native-window` fixture
changes are required.
