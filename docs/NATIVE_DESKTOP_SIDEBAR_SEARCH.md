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
