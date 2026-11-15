# Sidebar chat actions

Issue #98 completes the existing stream menu with Mark all as read, Rename and
Archive. The pinned workspace_ui sidebar context menu and messenger action
helpers define the behavior: read only when unread is nonzero, rename only for
ordinary channels with a nonempty changed name, and archive for channel/direct
chats after confirmation. Keep New topic and existing keyboard context-menu
entry points. Do not implement folder, pin or membership work from other issues.

Reuse current owned stream DTOs, stream context keys, canonical request checks,
cache projections and the archive drawer. Extend the existing bounded archive
operation mechanism for the opposite archive transition; preserve its uncertain
outcome read-back and newer-event authority. Rename owns only name/timestamp;
mark-read owns read markers/counters, never unrelated stream fields. Do not
navigate to a nonselected stream to execute its menu action. Owner, API runtime,
catalog, target ACL changes and external archive events retire stale UI intents.

Dialogs follow existing native form components and pinned source wording. Show
pending/error states and disable duplicate submission; Enter submits rename,
empty/unchanged input does nothing. Archive has distinct channel/direct wording,
the existing archive-history hint, and dismisses when the owned target leaves the
active list. Keep ordinary English/Russian and light/dark presentation.

Before product changes, reproduce the missing entries in the actual Main menu.
Add transport and actual-window checks for successful/failing actions, stale
owner/target completions, duplicate requests, keyboard/mouse access and the
archive/unarchive round trip. Retain New topic behavior when the menu grows.

## State and persistence

The controller exposes the current target's action availability, pending state
and error. Read and rename requests keep the existing owner, API generation,
catalog, cache lease and stream ACL fences. An ACL excursion permanently retires
the old request, even if the role later returns to its original value. Duplicate
submissions share the pending gate; at most 32 read/rename requests are retained.
While all slots hold valid pending requests, read and rename are unavailable in
the menu and confirmation form. A completed or invalidated request releases its
slot through the existing state notifications. Archive keeps its separate limit
and remains available when only the read/rename slots are full.
The most recent action owns its error, so a successful rename cannot display an
older archive failure. Names longer than 255 Unicode code points report the
existing translated channel-name validation error without sending a request.

Archive and restore share the existing bounded operation map, exact canonical
DTO checks and one GET reconciliation after an ambiguous POST. An explicit retry
of an uncertain operation first checks the same stream; it does not blindly
repeat a command. The confirmation names the target and explains that archiving
a channel affects all participants and archiving a direct chat affects both
participants. A stream archived elsewhere dismisses the dialog.

Rename projects only the canonical name and update timestamp. Read projects only
canonical counters and the message IDs observed before dispatch. A concurrent
newer stream event retains its name and counters; a message arriving during the
request remains unread. Loaded topic counters are cleared only when the stream
has no remaining unread and the topic projection has not changed. Cache history
and topics remain stale for canonical reload; exact observed message flags can
be patched in the existing cache. A successfully accepted read response also
publishes the existing conversation-read signal: saved viewport positions for
that stream are retired without moving the current viewport. Returning later
therefore opens the remaining unread boundary or the tail, rather than restoring
an obsolete position. Foreign, failed or invalidated replies cannot retire a
position. If a saved-position lookup or its around-anchor load is still pending
for that conversation, the accepted read retires its existing opening ticket.
The window resumes ordinary unread/tail selection after the read projection has
finished; a late old lookup cannot restore the discarded position. The same
scope check covers validated stream/topic read events, including an aggregate
view of that topic. Completed viewports, other conversations and explicit
message-link anchors retain their existing behavior. Neither action navigates
to its target.

The modal dialogs use the existing field and button components, suspend visible
message read acknowledgements, keep pending operations disabled, and retire on
owner/runtime/target changes. Russian action wording distinguishes the Archive
command from the existing Archive tab. Hidden menu actions are also disabled so
keyboard navigation cannot focus an invisible read or rename action.

## Regression coverage

Controller coverage exercises action success/refusal, complete foreign DTOs,
newer events, ACL changes and ABA, deletion/logout, repeated submission,
read/new-message races, and lost archive acknowledgements. Existing restore and
cache tests cover the shared archive operation implementation.

Actual Main window coverage uses real menu input and the HTTP fixture. It checks
mouse and both keyboard context-menu entry points, empty/unchanged rename,
Enter submission, read counter removal, channel/direct confirmation, pending and
failure states, error dismissal, external archive dismissal, and restore through
the existing archive drawer in English/Russian and light/dark themes. A real menu
read followed by leaving and returning reproduces the stale-position defect
before the signal integration; controller checks retain messages arriving during
the request and reject position invalidation from stale completions. The original
missing-action regression stays in the window suite. A delayed HTTP saved-anchor
lookup also reproduces the obsolete-window race for sidebar, stream and topic
bulk reads; a separate variant admits a new unread message during the held read
POST. All four cases fail before ticket retirement and check the visible final
tail or remaining unread target after the late lookup is released. Existing
opening cancellation, foreign/invalid responses, bounded retries and explicit
anchor checks cover the unchanged guards. Live-server and non-Linux acceptance are separate from these local synthetic transport/window checks.
