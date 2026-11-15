# Attachment downloads

## Design and scope

Issues #74 and #193 share one prerequisite: an owned list of attachment saves.
Previously the controller exposed only a transient notice. It now retains
small in-memory download records in the existing controller and continues
to use `FileClient` for authenticated metadata, streamed bytes, checksum checks,
atomic saves and cancellation. No additional transport or queue is introduced.

The top-bar download center follows the pinned desktop reference's 320px panel,
256px scrolling list, active-count badge, terminal-only Clear and row actions.
Rows expose starting, downloading, downloaded and error states. Active rows can
be cancelled; successful rows can be opened or revealed; terminal rows can be
removed. Failed rows can retry without finding the attachment in the chat again.
Names and destinations remain stable across a retry, except that existing files, including dangling symbolic links,
and destinations reserved by active downloads require a unique numeric suffix.

Destination selection is advisory: another process can create that name during
the transfer. Download-center saves therefore use FileClient's explicit
KeepExisting mode: create a private staging directory on the target filesystem
and write directly to an exclusively created, held file. POSIX creates the file
with `openat` through a verified owner/0700 directory descriptor. It never reopens
the directory pathname to write or commit. Linux publishes the held file
through `/proc/self/fd`; other POSIX platforms use `linkat` with the pinned
directory descriptor. A writer of the parent may rename that directory, but
another UID cannot replace its contents or redirect the held descriptor. The
same UID already controls the user's files and is not an isolation boundary.
Windows creates the file with `CREATE_NEW`, DELETE access and exclusive sharing,
then publishes that same handle with `FileRenameInfo` and replacement disabled.
It closes the exclusive handle before invoking completion consumers.

Staging cleanup disables QTemporaryDir's recursive removal. It deletes only the
held directory's fixed child with `unlinkat` on POSIX, or the held file through
`FileDispositionInfo` on Windows, then attempts a nonrecursive empty-directory
removal. A substituted directory's contents and symlink targets are never
traversed. Renaming a staging directory can leave an empty directory at the
attacker-selected new name; cleanup does not search for it by pathname.
Unsupported filesystems or unavailable safe operations report an error rather
than falling back to pathname replacement. Existing FileClient callers keep their
replacement behavior. Retry retains the original requested path separately from
the allocated attempt path, including when allocation failed before transfer.

Each attempt has its own id. Retry first uses the existing Session refresh and
waits for verified authentication. A transient refresh failure retains the old
error row. Only a still-owned error row can be removed and restarted; a failed
restart belongs to the new row. Native row removal is a synchronous local
operation, rather than the reference's fallible Electron IPC dismiss call.
Removing a row while refresh is pending must prevent its restart.

Actual owner loss clears rows and cancels active work. Credential replacement
retires active transfers, retaining their same-owner error records for explicit
retry. Late callbacks cannot update successor rows. Terminal records and local
paths are not persisted or restored across sign-out or process restart.

## Validation

The initial local HTTP regression failed because a completed HTTP 503 save left
no row. The initial visible Qt Quick regression failed because the failed file
had no download-center trigger. Both pass with the implementation.

Controller checks exercise metadata/starting and binary/downloading boundaries,
byte counters, cancellation, deduplication, active destination reservations,
existing-file preservation and terminal cleanup without deleting saved files.
A separate candidate regression reproduced treating a dangling symbolic link as
an available name; the final allocator preserves both the link and its target.
Retry checks hold the refresh and metadata replies independently. They verify a
new attempt id with the same file UUID/name, the refreshed bearer, unchanged-token
refresh, refresh/restart errors, removal during refresh, service replacement and
logout. Refresh failure retains the original failed row. The local dismiss is
not an IPC call and has no artificial rejection seam: an absent or no-longer-owned
row cannot be restarted.

A Linux syscall-boundary regression first reproduced an unverified file and a
symbolic link being substituted after commit but before publication. The test
interposes only that syscall in an isolated child and performs real filesystem
renames; there are no production test hooks. Both substitutions now publish the
verified bytes. A separate check executes the POSIX directory-descriptor backend
on Linux after renaming the entire staging directory and replacing its old path,
and confirms that a competing destination is preserved.

Two further regressions reproduced recursive cleanup deleting an unrelated file
inside a substituted directory, and a symlink redirect overwriting an unrelated
`payload` during commit. They now preserve the unrelated files and complete the
verified download without reopening or recursively cleaning staging paths.

The Windows implementation follows the documented
[CreateFile](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-createfilew)
and [FILE_RENAME_INFO](https://learn.microsoft.com/en-us/windows/win32/api/winbase/ns-winbase-file_rename_info)
semantics. Windows runtime and native macOS acceptance remain **NOT RUN**; the
POSIX backend's Linux check is not a macOS acceptance claim.

The complete FileClient suite also checks progress on a real streamed body and
cancellation/destruction inside initial and partial progress callbacks. Existing
atomic-save, checksum, size, deadline and owner-generation checks still apply.
Open dispatches the exact local URL through QDesktopServices. Linux reveal sends
that URL through org.freedesktop.FileManager1.ShowItems; its controller test runs
on a private D-Bus session and never replaces the desktop file manager.

Visible English/Russian light/dark checks use real pointer actions through the
panel, a failed refresh followed by a successful retry, accessible numeric
progress, cancel, ready and remove states. The existing visible upload/save-dialog
scenario also passes. Native icon resource packaging and Popup accessibility
errors found in the first candidate run were corrected before acceptance.

This is local synthetic HTTP and visible Linux Qt acceptance. Complete controller
and window suites, live Workspace, the operating system's real file manager and
native picker, packaged installation, Windows/macOS and a full matched-pixel
reference comparison were not run. The reference contracts are the pinned
workspace_ui `workspace-file-download.lib.ts`, `workspace-downloads.ts`,
`top-bar-download-center.ui.tsx` and `top-bar-download-row.ui.tsx`.
