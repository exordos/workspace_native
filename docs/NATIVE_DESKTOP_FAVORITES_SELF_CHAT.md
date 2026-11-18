# Favorites page: the self-chat "Personal notes" conversation

Issue #70 ports the reference "Favorites" activity route: the sidebar Favorites
chip opens the user's own direct stream (a private direct stream with
`direct_user_uuid` equal to the signed-in user, created as "Personal notes")
and presents it as the Favorites conversation. Design fidelity case:
parity-preserving port, no redesign.

Reference contract: workspace_ui `61dacef78dab2f5d0be76f283bebf49fe29b84af`.
Figma token unavailable - mockups not consulted; the code oracle above is the
only fidelity source.

## Reference behaviour (cited)

- Route: `packages/web/src/app/app-route-definitions.tsx:176` -
  `project/:projectId/activity/favorites` renders `FavoritesPage`.
- Page and states: `packages/web/src/pages/chat/chat-page.ui.tsx:21-72`.
  - `ready` renders the ordinary conversation with `presentation="favorites"`
    (line 43).
  - `loading` renders the favorites header plus a centered `Spinner` with
    `t("app.loading")` (lines 50-54).
  - `error` renders a bordered card with `t("favorites.loadError")` and a
    `t("app.retry")` button (lines 56-65).
  - The right drawer is forced closed on mount
    (`chat-page.ui.tsx:40-42`, `useCloseFavoritesRightDrawer`).
  - The favorites header is `ChatFavoritesHeader`
    (`packages/web/src/widgets/chat-view/chat-header-favorites.ui.tsx:5-10`):
    the shared header shell with only an `<h1>` titled `t("activity.favorites")`
    - no call/search/panel actions.
- Self-chat bootstrap: `packages/web/src/features/workspace-self-chat/workspace-self-chat.hook.ts`.
  - View states `loading | ready | error` with `retry` (lines 21-23).
  - Deduplication: one in-flight ensure per runtime generation, keyed
    `ownerKey:generation:<n>` in `ensureRequestsByRuntimeKey`
    (lines 36, 42-44, 48-56); the entry is dropped once the promise settles
    (lines 63-68), so a retry starts a new request while concurrent callers
    share one request.
  - The ensure creates a direct stream with self:
    `createWorkspaceDirectStream({ directUserUuid: runtimeContext.userUuid,
    name: "Personal notes", description: "" })` (lines 53-60); the wire body is
    `{ name, description, source_name: "native", direct_user_uuid }`
    (`entities/messenger/messenger-create-chat-actions.lib.ts:202-217, 484-544`).
  - A self stream already in the catalog plus its default topic short-circuits
    the request (`findWorkspaceSelfChatStream` / `findWorkspaceDefaultTopic`,
    effect guard at lines 130-190); `ready` carries the
    `{streamUuid, topicUuid}` topic route.
- Self-chat detection: `packages/web/src/entities/messenger/messenger-self-chat.lib.ts:8-18` -
  a stream/conversation is self-chat iff `isPrivate && directUserUuid ===
  currentUserUuid`; the default topic is the stream's `isDefault` topic
  (lines 34-43).
- Ordinary chat lists must omit the self stream (clarifying comment, matrix
  sweep at workspace_ui 75bd21f37):
  `packages/web/src/entities/messenger/messenger-sidebar.lib.ts:560` (folder
  membership rows), `:592` (folder conversation rows), `:616` (stream list),
  `:656` (All-folder catch-up pass) and
  `packages/web/src/widgets/sidebar/sidebar-shell.ui.tsx:124` (shell-level
  total count filter).
- Copy (pinned `en`/`ru` locale files at the contract commit):
  `activity.favorites` = "Favorites" / "Избранное"; `favorites.loadError` =
  "Could not open Favorites." / "Не удалось открыть Избранное."; `app.loading` =
  "Loading…" / "Загрузка…"; `app.retry` = "Try again" / "Попробовать снова".

## Native design

### Controller (`packages/native/src/native-controller.{h,cpp}`)

- `Q_PROPERTY(bool favoritesVisible ...)`, `Q_PROPERTY(QString favoritesStatus
  ...)` ("" | "loading" | "ready" | "error"), `Q_PROPERTY(QString favoritesError
  ...)`, all on the existing `stateChanged` notification.
- `Q_INVOKABLE void showFavorites()`: requires an authenticated session; closes
  the other content pages (new `ContentPage::Favorites` slot so the drafts,
  mentions, inbox, feed, starred and reactions pages leave), then resolves the
  self stream:
  1. catalog hit (`privateStream && directUser == session user`, DTO
     `default_topic_uuid` valid) -> select the stream and default topic and go
     `ready` with no HTTP create;
  2. otherwise `loading` + ensure.
- `Q_INVOKABLE void retryFavorites()`: clears the error and re-runs ensure
  (reference `retry` resets request state, hook lines 125-128).
- `Q_INVOKABLE void closeFavorites()`: leaving the favorites page (a normal
  stream/topic selection also clears it, mirroring route unmount).
- ensure ("ensure-self-stream"): one `POST /messenger/streams/` with
  `{name: "Personal notes", description: "", source_name: "native",
  direct_user_uuid: <current user>}` - the same body shape as the existing
  peer direct-chat create (`postDirectChat`). In-flight requests are deduped
  per runtime generation with key `creationOwnerKey() + ":generation:" +
  m_api->generation()` (reference `runtimeRequestKey`), the entry is dropped
  when the request settles, and repeated calls reuse the in-flight request.
  Responses are fenced on owner + API generation like the direct-chat flow.
  On success the parsed stream is validated with the reference rule (`private`
  and `direct_user_uuid == self`, else a contract-mismatch error), upserted
  into the stream catalog (reference `upsertStream`), and opened; the default
  topic comes from the DTO, with one `GET /stream_topics/?stream_uuid=` pass as
  the fallback (reference `findWorkspaceDefaultTopic`).
- List filtering: `NativeController::sidebarStreams()` drops self-chat rows,
  and `streams()` keeps them. The reference filters the sidebar projections, not
  the catalog (`messenger-sidebar.lib.ts:560,592,616,656`); keeping `streams()`
  complete is what lets header title, encryption and unread lookups resolve the
  selected self stream while the ordinary lists stay clean. `isFavoritesSelfStream`
  is the single predicate used by both the filter and the ensure flow.
- `favoritesError` surfaces the reference copy: `tr("Could not open
  Favorites.")` (favorites.loadError). `app.loading` maps to the existing
  `tr("Loading…")` catalog entry; `app.retry` maps to `tr("Try again")`.

### QML (`packages/native/qml/Main.qml`, new `NativeFavoritesPage.qml`)

- The compact rail chip `activity-favorites` becomes enabled and calls
  `controller.showFavorites()`; the expanded `activityRow-favorites` row
  likewise loses its `modelData.id !== "favorites"` disabled predicate.
  `selected`/`routeActive` follow `controller.favoritesVisible`.
- `NativeFavoritesPage.qml` (sibling of `NativeDraftsPage` etc.) is visible
  while `favoritesVisible && favoritesStatus !== "ready"`: the favorites header
  title plus either the loading chip (`BusyIndicator` + "Loading…") or the
  bordered error card with the Retry button (`favoritesRetryButton`).
- When `favoritesStatus === "ready"` the ordinary conversation column renders
  the self stream conversation; the conversation header swaps its title/actions
  for the favorites header (title-only "Favorites", objectName
  `favoritesConversationHeader`) and the header action row is hidden.
- The right drawer is forced closed on the false -> true transition of
  `favoritesVisible` (`rightDrawer.close()`), matching the reference mount
  effect.
- Composer behaviour is unchanged: the favorites conversation is a selected
  topic, so sending works exactly as in a normal DM.

### i18n

Every new `tr()`/`qsTr()` string gets a reviewed Russian entry in
`packages/native/translations/workspace_ru.ts` (the locale CTest enforces
catalog completeness): the favorites header title
(`activity.favorites` equivalent "Избранное"), "Could not open Favorites."
("Не удалось открыть Избранное.") and "Try again" ("Попробовать снова");
"Loading…" is already catalogued.

## Tests (TDD, RED -> GREEN per slice)

- `native-native-controller` (tests/native-controller-test.cpp):
  - `favoritesEnsureCreatesSelfStreamOnceAndReuses` - empty catalog:
    `showFavorites` dispatches exactly one `POST /streams/` with the reference
    body (name "Personal notes", `direct_user_uuid` = self), lands `ready` on
    the default topic; repeated calls and a second `showFavorites` make no
    additional POSTs; the create body is `source_name: "native"`.
  - `favoritesEnsureDeduplicatesConcurrentCallsPerGeneration` - two back-to-back
    `showFavorites` calls while the POST is held share one request; the settled
    retry issues a fresh request.
  - `favoritesEnsureFailureSetsErrorAndRetryRecovers` - POST 500 -> `error`
    status with the reference copy; `retryFavorites` succeeds and opens.
  - `favoritesSelfStreamIsHiddenFromOrdinaryLists` - a catalog with a
    "Personal notes" self stream: `sidebarStreams()` omits it while an
    ordinary stream remains; `streams()` still carries it for lookup purposes;
    folder membership projection also omits it; opening Favorites uses the
    catalog entry with zero POSTs.
- `native-native-window` (tests/native-window-test.cpp, inside
  `NativeWindowTest`):
  - `favoritesChipOpensSelfChatConversation` - the enabled chip opens the
    favorites conversation: favorites header title, the self stream's message
    renders, the composer behaves as in a normal DM (real click, type, send,
    bubble visible), the self stream row is absent from the sidebar list, and a
    previously open right drawer is closed.
  - `favoritesEnsureFailureShowsErrorCardAndRetry` - held/failing create shows
    the error card with Retry; Retry recovers to the conversation.
- Window cases run inside the whole `native-native-window` CTest (the group
  executes the full binary, so no per-function registration is needed).

## Live verification

Before ready_for_review, per the lane contract: login on :33 via
`live_qa.py native-login`, open Favorites via the chip, send a
`CASSI QA <UTC>` message into Personal notes, confirm the self stream is
absent from the ordinary stream list, then the health/send latency gates. If
the live server rejects direct-self creation the PR reports `blocked` with the
exact server error instead of a fake pass.
