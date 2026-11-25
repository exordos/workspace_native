# Sidebar search matching

Issue #99 extends the existing sidebar query without changing server search or
conversation authority. The pinned workspace_ui reference is
`61dacef78dab2f5d0be76f283bebf49fe29b84af`:
`widgets/sidebar/sidebar-filtering.lib.ts`, `sidebar-workspace-search.lib.ts`,
`sidebar-workspace.ui.tsx`, and `shared/lib/topic-display.lib.ts`.

## Design

Trim and lowercase the query once. Keep the current folder's stable catalog
order and match a stream's name, or any topic's displayed title (including its
leading #). For direct chats, also match the known participants' names and
emails from the existing owned user directory. Empty/whitespace input restores
the ordinary sidebar. Search exposes only matching topics and expands them
without changing the selected conversation. Opening a matching topic uses the
existing canonical topic-opening route with its stream hint.

Reuse the Inbox topic slots and their existing paginated, owner/generation-bound
hydration, move reconciliation and deletion tombstones. Starting a nonempty
search hydrates nonarchived channel catalogs, including channels never opened
in this session. The projection remains presentation data; a search match never
makes a cached topic authoritative for navigation. No separate search index,
transfer queue, disk schema or per-keystroke server search is introduced.

This change implements matching within the existing folder projection. The
cross-folder result sections and All folders separator remain folder-layout
work; matching does not reorder or move a stream between folders.

## Verification plan

Before production changes, an actual Main.qml window fixture must fail for an
unopened channel's topic, displayed # title, a direct participant's email and a
trimmed query. Check both palettes and locales. The same fixture checks names,
nonmatches, stable row order while typing, search reset and canonical cross-stream
topic navigation after the change. Run targeted existing folder and topic-opening
controls and the production build. Synthetic local HTTP/UI checks do not establish
live Workspace, installed-package, Wayland or Windows/macOS acceptance.

## Local results

The unchanged application failed 20 matching assertions across EN/RU and
light/dark; all four name-only controls passed. The final visible Linux XCB
window run passed all 24 search cases and seven existing folder/canonical-topic
navigation cases. The order probe uses `plan` before `planning`: its original
`pl` prefix also correctly matched the fixture participant's `example.invalid`
email and therefore was not a valid unmatched-DM oracle.

Three controller cases cover held hydration, per-query request deduplication,
logout before delivery, topic rename/deletion and participant email-only updates.
Those and nine existing Inbox lifecycle/move/deletion/retry cases passed. The
production and test targets build successfully. The full window/controller
suites, live Workspace, installed package and other platforms were not run.

# Issue #847: cross-folder results and separator rows (re-baseline e0194ffb)

The deferral above was closed by the reference in 75bd21f (issue #428). This
section re-baselines the sidebar search against workspace_ui
`e0194ffb667986b58a12256ce41ca5dc99b56b83`:
`widgets/sidebar/sidebar-workspace-search.lib.ts` and
`widgets/sidebar/sidebar-workspace.ui.tsx`.

## Reference contract (measured at the contract sha)

Projection (`projectWorkspaceSidebarSearch`, sidebar-workspace-search.lib.ts):
- `localStreams`: query matches inside the selected folder, stable folder order.
- `globalStreams`: query matches across ALL streams, deduplicated against the
  local section by `streamUuid`; order is the workspace catalog order.
- Selected folder with `systemType === "all"`: local is empty and every match
  renders in the global section (no local/global split to separate).

Search results container (`sidebar-workspace.ui.tsx:659-689`, the
`[data-sidebar-search-results]` div) renders, in order:
1. local stream rows (`renderStreams(localStreams, ...)`),
2. a muted status line (`role="status"`, classes `px-2 py-2 text-xs
   text-text-muted`) with `sidebar.noMatchesInFolder` when not loading, the
   selected folder is not "all", local is empty and global has matches,
3. when local and global both have matches and the folder is not "all": a
   separator row (`role="separator"`, `aria-label = sidebar.allFoldersResults`,
   classes `flex items-center gap-2 px-2 pb-1 pt-3`) whose children are a
   hairline `bg-border-subtle/70 h-px flex-1`, the muted `SectionLabel`, then a
   second hairline (label centred between two rules),
4. global stream rows.

Topic-level separator ("Other topics", `sidebar-workspace.ui.tsx:314-328`):
inside a stream's search-mode topic list. Matching topics render first; when
the stream still has non-matching topics the list offers the same
show-more/show-less control as the ordinary sidebar (reference
`WorkspaceSidebarSearchTopics`: `hiddenCount = stream.topics.length -
matchingTopics.length`, toggle action when `hiddenCount > 0`). When expanded
and at least one topic matched by its own title, index `matchingTopics.length`
is preceded by a separator row: `role="separator"`, `aria-label =
sidebar.otherTopics`, classes `flex items-center gap-2 px-2 py-1.5`, a muted
`SectionLabel` with the text followed by a trailing hairline
`bg-border-subtle/70 h-px flex-1` (rule fills the rest of the row).

Keyboard navigation from #628 keeps its contract: the arrow ring is built only
from `[data-sidebar-search-item]` rows, so separator rows are never focusable
targets.

i18n keys (both locales required): `sidebar.otherTopics` en "Other topics" /
ru "Остальные темы"; `sidebar.allFoldersResults` en "All folders" /
ru "Все папки"; `sidebar.noMatchesInFolder` en "No matches in this folder" /
ru "В этой папке совпадений нет". Search-header a11y labels `sidebar.folderList`
("Folder list" / "Список папок") and `sidebar.chatSearch` ("Search chats" /
"Поиск чатов").

## Native design

- C++ (`NativeController::sidebarStreams`): while a query is active, compute the
  local section exactly as today (folder-scoped filter) and a global section
  from the full catalog order with dedup by stream id against local. The
  returned `QVariantList` stays flat: a marker row `{ "searchSeparator":
  "allFolders" }` is inserted between the sections only when the selected
  folder is not "all" and both sections are non-empty; a status marker row
  `{ "searchStatus": "noMatchesInFolder" }` is inserted when local is empty,
  global is non-empty and the folder is not "all". The "all" system folder
  projects matches from the full catalog with no separator (matching
  `globalStreams`-only rendering). Marker rows carry no `id` and delegate all
  stream-only consumers to the existing `stream-`/`topic-` objectNames.
- QML: the sidebar Repeater branches on the marker rows. The all-folders
  separator renders as a row of height 28 (py-1.5 + 13 px label) with the
  hairline rule token `border-subtle` at 70 percent opacity; the label uses
  `text-muted` at the section-label size. Topic rows keep their current model;
  search mode gains the reference's show-more behaviour: `searchTopics` first,
  remaining topics only after expand, with the "Other topics" separator row
  (label then trailing hairline) as the first item of the non-matching tail.
  Expanded state is per-stream presentation state only and resets when the
  query changes, mirroring the reference component's local `useState`.
- `sidebarSearchTargets()` skips marker rows, so ArrowUp/Down and Escape keep
  working and never focus a separator.
- New `tr()` strings (`Other topics`, `All folders`, `No matches in this
  folder`) are added to `packages/native/translations/workspace_ru.ts` in the
  same PR (catalog guard).

## Verification plan

Window cases (both palettes and locales) in `native-window`: separator appears
between local and global sections with the exact label and hairline geometry;
global section deduplicates streams already present locally; the "all" folder
projects global matches only and never renders the all-folders separator; the
no-matches-in-folder status line appears only when local is empty; the "Other
topics" separator appears only after expanding a stream whose topic list has
title matches plus a non-matching tail, and never when nothing matched by
title alone; arrow navigation from #628 skips every separator row. Controller
cases pin the projection directly: local/global split, dedup, "all"
systemType, empty query restoring the ordinary folder sidebar. Live
verification on workspace.exordos.com with queries spanning two folders.
