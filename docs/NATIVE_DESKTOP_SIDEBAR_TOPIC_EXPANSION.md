# Sidebar topic expansion

Issue #95 follows `workspace_ui` master at `75bd21f37de94728e2ebe100acfea24ac9c1a4e1`:
`sidebar-workspace.ui.tsx`, `sidebar-chat-row-meta.ui.tsx`,
`sidebar-topic-collapse.{lib,hook}.ts`, `sidebar-topic-show-more.ui.tsx` and
`sidebar-chat-row-layout.lib.ts`. Its normal-density action slot overlays the
unread badge on hover or when keyboard focus is on the row, its avatar, or its
expansion control. The chevron toggles a stream without
navigating, independently of the other expanded streams. Selecting a stream or
topic keeps only that stream expanded. Expansion is scoped to the current server,
project and user and stored using the existing native preferences mechanism.

The existing owner-fenced background topic catalog supplies nonselected streams.
Topic rows retain their current native order (unfinished before completed).
Initially the first three are shown, including completed topics if there are fewer
than three unfinished. The next action reveals remaining unfinished topics,
then remaining completed topics, then collapses the extras; labels show the
number revealed by the next action. Revealing focuses the first newly visible
topic. A collapsed active topic highlights the stream, whereas an expanded
active topic highlights its row. Search retains its existing native behavior.

Background expansion limits its own concurrent topic requests to two; foreground
navigation and search retain their existing independent paths. Topic hydration
updates the affected sidebar row and the Inbox when it is visible, without
republishing unrelated window state for every background response. A saved
snapshot remains visible if revalidation fails; a later explicit interaction
can retry it. A deferred cache result does not release a still-running request.

Synthetic Linux window checks exercise expansion, restart, reveal levels and
focus. The restored-history window fixture also returns the canonical read
receipt expected from the server and checks that the client accepts it without
an error. Live service and other-platform acceptance are separate.
