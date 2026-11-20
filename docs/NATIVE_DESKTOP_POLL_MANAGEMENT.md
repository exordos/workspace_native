# Managing a poll

The bubble menu offers Poll settings and Close poll on an open, unexpired poll
authored by the signed-in account in a supported stream. The menu closes before
either dialog opens. The accepted scope of issue #231 is author management.

Closing a poll requires confirmation. Settings show the first question, the
original optional deadline and the permission to add options. Removing the
deadline saves a null timestamp. A deadline that passes after opening the form,
or a new time in the past, prevents saving. Cancelling discards edits. On a
successful save or close the existing controller applies the returned poll
revision to the bubble.

Both dialogs retain their form on a server error. They disable dismissal and
repeat submission while a request is pending. Ownership, route and message
changes retire the dialog so a response cannot settle a different poll's form.

The pinned reference is the poll bubble menu, the poll-specific bubble and
`workspace-poll-management-dialogs.ui.tsx` at workspace_ui
`230e02b8b0273dee556a406746d122180a68b3e7`. The existing native controller
implements the close and settings requests. Window tests cover the actual menu,
pending and retry states, validation and payloads in ordinary poll sessions.

The Workspace backend derives the actor from the authenticated request, checks
active membership and the message placement, and enforces poll action permissions
in `workspace/messenger_api/api/poll_store.py`. Both that backend and the pinned
web UI also permit stream administrators, owners and moderators to manage peers'
polls. The native menu is limited to an author's own poll by #231. The server
still decides whether an authenticated request succeeds.
