# Sidebar catalog states: loading, blocking error, partial error, empty states

Issue #100. Parity-preserving. All reference measurements were taken from
`workspace_ui` at the pinned sha `61dacef78dab2f5d0be76f283bebf49fe29b84af`
(read over `git show`, file `packages/web/src/widgets/sidebar/sidebar-workspace.ui.tsx`).
Figma token unavailable - mockups not consulted.

## Contract measured from the reference

The issue cites `sidebar-workspace.ui.tsx:3650-3693`; at the pinned sha the file
is 893 lines and the same contract lives at the line numbers measured below
(the issue numbers refer to a later master revision of the same file).

### State machine (`sidebar-workspace.ui.tsx:662-699`)

```
searchMode            = normalizedQuery.length > 0
displayedStreamCount  = searchMode ? local+global search hits : streams.length
hasSpecificEmptyContext = searchMode || workspaceStreamCount > 0
showLoading           = loading && displayedStreamCount === 0            (827)
showBlockingError     = error && displayed === 0 && !hasSpecificEmptyContext (697)
showNonBlockingError  = error && (displayed > 0 || hasSpecificEmptyContext)  (695)
showEmptyState        = !loading && displayed === 0 && (!error || hasSpecificEmptyContext) (698)
```

The shell (`sidebar-shell.ui.tsx:226-233, 263-272`) feeds:
`loading = bootstrap isLoading || (!catalogLoaded && error == null)`,
`error = messenger.error` (the catalog model error),
`workspaceStreamCount` = total workspace stream ids (not folder-scoped), and
`selectedFolderSystemType` = the effective folder's `systemType`.

### Empty-state resolver (`sidebar-workspace.ui.tsx:140-177`)

| condition | title key | EN | hint key | EN hint |
| --- | --- | --- | --- | --- |
| active query | `sidebar.emptySearch` | No matches | `sidebar.emptySearchHint` | Try a different chat or topic name. |
| `workspaceStreamCount === 0` | `sidebar.emptyWorkspace` | No workspace chats yet | `sidebar.emptyWorkspaceHint` | When Workspace conversations load, they will show up here. |
| folder `systemType === "personal"` | `sidebar.emptyPersonalChats` | No direct messages yet | `sidebar.emptyPersonalChatsHint` | Start a DM from the compose menu or directory. |
| folder `systemType === "channels"` | `sidebar.emptyChannelList` | No channels yet | `sidebar.emptyChannelListHint` | Subscribe to streams you care about to see them here. |
| otherwise (custom folder) | `sidebar.emptySelectedFolder` | Folder is empty | `sidebar.emptySelectedFolderHint` | Add chats from a chat menu to populate this folder. |

RU (en.json 1467-1479 / ru.json 1478-1490 at the sha):

| key | RU |
| --- | --- |
| emptySearch | Ничего не найдено |
| emptySearchHint | Попробуйте другое название чата или темы. |
| emptyWorkspace | В Workspace пока нет чатов |
| emptyWorkspaceHint | Когда чаты Workspace загрузятся, они появятся здесь. |
| emptyPersonalChats | Пока нет личных сообщений |
| emptyPersonalChatsHint | Начните личный диалог через меню создания чата или каталог. |
| emptyChannelList | Пока нет каналов |
| emptyChannelListHint | Подпишитесь на интересующие каналы, чтобы они отображались здесь. |
| emptySelectedFolder | Папка пуста |
| emptySelectedFolderHint | Добавьте чаты через меню чата, чтобы заполнить папку. |
| partialLoadError | Часть данных боковой панели не обновилась |
| app.loading (`app.json`) | Загрузка… |
| app.error (`app.json`) | Что-то пошло не так |

The header strings: `app.loading` = "Loading…" (en.json, ellipsis U+2026),
`app.error` = "Something went wrong", `sidebar.partialLoadError` =
"Some sidebar data could not be refreshed".

### Blocks and geometry (reference Tailwind -> px, `sidebar-workspace.ui.tsx:827-870`)

All blocks sit inside the sidebar scroll area, between the activity section
and the stream list. Outer wrapper of the cards is `px-3 py-4` (12/16), the
partial banner wrapper is `px-3 pb-2` (12 left/right, 8 bottom).

Loading card (`827-838`, `role=status`, `aria-busy`): dashed card
(`rounded-lg border border-dashed border-border-subtle`, bg `bg-bg-elevated/40`),
`flex-col items-center gap-3 px-3 py-6`; `Spinner size="lg"` = 32x32 px ring,
`border-2` (2 px), full ring `border-border-subtle`, top edge `border-t-accent`
(`spinner.ui.tsx:4-15`, lg = `h-8 w-8`), continuous spin animation; label
`text-sm text-text-muted` = `app.loading`.

Blocking error card (`840-848`): same dashed card with `gap-2 px-3 py-5`;
`Icon name="info" size={18}` tinted `text-notice-base`; title `text-sm font-medium
text-text-primary` = `app.error`; error text `max-w-[220px] text-xs text-text-muted`
centered.

Partial error banner (`849-861`): solid card (`rounded-lg border
border-border-subtle`, bg `bg-bg-elevated/60`), `flex items-start gap-2 px-3 py-2`;
`Icon name="info" size={16} mt-0.5` tinted `text-notice-base`; title
`text-xs font-medium text-text-primary` = `sidebar.partialLoadError`; the error
line is `truncate text-xs text-text-muted`.

Empty card (`862-870`): same dashed card as the error card (`gap-2 px-3 py-5`);
`Icon name="chatBubble" size={18}` tinted `text-text-muted`; title
`text-sm font-medium text-text-primary`; hint `max-w-[220px] text-xs
text-text-muted`.

### Palette hex (blue-cold, `packages/web/src/shared/lib/themes/blue-cold.ts`)

| token | light | dark |
| --- | --- | --- |
| bg-elevated | #ffffff | #222328 |
| text-primary | #1b1b1d | #ffffff |
| text-muted | #989898 | #707070 |
| border-subtle | #d8e4ef | #33384d |
| notice-base | #7087ff | #3d5eff |
| accent | #7087ff | #7087ff |

## Native design

Single source of truth in the controller: a new read-only projection
`NativeController::sidebarCatalogState` (QVariantMap, NOTIFY
`sidebarCatalogStateChanged`) that mirrors the reference derivation one-to-one:

- `loading` := authenticated, catalog not yet verified (`m_catalogVerified`,
  the measured native catalogLoaded, see also the inbox derivation at
  `native-controller.cpp:7801-7805`) and no catalog error pending.
- `error` := a new dedicated `m_sidebarError` field fed only by the catalog
  (streams) load: set to the same text `collection()` reports
  (`Could not load the catalog. <statusError>` / `The server returned an
  invalid channel list.`) when the streams collection walk fails, cleared on
  every `reloadSnapshot()` and on an authoritative delivery.
- `displayedStreamCount` := `sidebarStreams().size()` (already search- and
  folder-scoped natively), `workspaceStreamCount` := `m_streams.size()`,
  `selectedFolderSystemType` := `systemType` of `selectedFolder()`.

Fields: `showLoading`, `showBlockingError`, `showPartialError`, `showEmpty`,
`errorText`, `loadingText`, `errorTitle`, `partialTitle`, `emptyTitle`,
`emptyHint` (localized with tr(), EN source strings exactly as measured, RU
entries added to `translations/workspace_ru.ts`).

QML (`Main.qml` sidebar): a block column at the current stream-list top
(`y = 60` in search mode, `104` otherwise) rendering at most one dashed card
(loading | blocking error | empty) plus the partial banner above the list;
`streamList.anchors.topMargin` grows by the block height. objectNames:
`sidebarLoadingCard`/`sidebarLoadingSpinner`/`sidebarLoadingText`,
`sidebarErrorCard`/`sidebarErrorTitle`/`sidebarErrorBody`,
`sidebarPartialBanner`/`sidebarPartialTitle`/`sidebarPartialLine`,
`sidebarEmptyCard`/`sidebarEmptyTitle`/`sidebarEmptyHint`.

Native folder support (#83) is real: server folders carry `system_type`
`all|personal|channels|created` and scope the sidebar via `folder_items`, so
the folder empty states are reachable now, not stubbed.

## Deviations

- Dashed card border: `Rectangle.border.dashed` (Qt >= 6.8) renders the CSS
  `border-dashed`; 1 px border-subtle, radius 8.
- Spinner: drawn with the repo's Canvas pattern (`StatusSpinner`, Main.qml) -
  32 px ring, 2 px stroke: full ring border-subtle + top-half accent arc,
  1 s rotation. CSS `border-t-accent` colors only the top quarter; the arc
  covers the top half, visually equivalent while rotating.
- The reference blocking card shows the catalog model's raw `error` string;
  native shows the controller's catalog-load text
  (`Could not load the catalog. Request failed (HTTP 503).`), which carries
  the server status message. RU: `Не удалось загрузить каталог. …`.
- Native `loading` is `!m_catalogVerified && no catalog error`; the reference
  additionally ORs the bootstrap `isLoading` flag. Because `reloadSnapshot()`
  clears the error at retry start, the "in-flight with stale error" edge that
  distinguishes the two never renders a different card: with rows visible no
  loading card is possible in either derivation.
- Empty-state resolver and the state machine land now for all five states;
  the folder rows land with #83, which has already merged, so all five are
  also covered by window tests.
