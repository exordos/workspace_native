# Keep saved Workspace sessions live across window switches

**Status** Proposed for review.

## Context

The session switcher used to retire the current transport and notification
routes whenever the user changed organizations. A warm return repeated login
and catalog loading while the account was unable to receive realtime events.

## Decision

`WorkspaceSessions` owns one verified session, API transport, realtime cursor
and background projection per signed-in account. The account key is the
vault identity for the exact HTTPS origin, project and user. The app owns one
visible controller and one set of media, drafts, cache and chat-key services.
The visible session uses the selected runtime's verified access credential.
The owning runtime schedules renewal and persists rotated credentials.

Window selection publishes the selected runtime's known sidebar catalog and
lets the visible controller reconcile it with the server. Background runtimes
use the existing C++ projection and skip the sidebar preview sweep and presence
heartbeat. Catch-up and reconnect use the existing realtime cursor contract
for each account.

Notification policy uses the message and its owning catalog. Selecting another
account leaves the notification route live. An activation selects the producing
account and feeds the exact message UUID to the standard message-anchor resolver.
Read and deletion events close that owner's notifications. Account removal or
invalidation retires that runtime and its notification routes.

## Consequences

Each remembered account uses independent realtime and catalog resources while
the app runs. The visible controller maintains its own authorized snapshot and
may perform catalog requests independently of the background projection.
Measure switch cost and background request load on the Linux reference stand
before changing the account or projection budgets.

The controller regression uses two TLS origins with the same project, user
and message UUIDs. It drives both sockets, clicks notifications, checks the
message anchor and verifies catch-up and account removal.
