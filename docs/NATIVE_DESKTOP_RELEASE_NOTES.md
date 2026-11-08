# Workspace Native 0.1.2 — Linux development preview

This update enables Unicode emoji selection in the message composer, including
search and repeated choices. A selection appends to the draft end, restores the
caret and focus, and keeps the panel open. Ordinary and encrypted text use their
existing send paths. Changing the conversation or session closes the old panel.
Sticker selection and the complete Source popup presentation remain unfinished.

The complete current window suite passed 278 cases. Focused checks include real
mouse/keyboard input, an independent Source encrypted-message reader and the
existing send/locked-key controls. These use synthetic local services; complete
live-account and desktop feature/pixel acceptance remain in progress.

The 0.1.1 updater automatically downloads a found update and offers an explicit
**Install and restart** action. The actual public 0.1.1 → 0.1.2 upgrade passed
on the current Ubuntu/Wayland host: automatic download, one explicit installation
action, normal restart into the exact released binary and one preserved synthetic
cache row. The new version first showed a metadata-check error; one real manual
check recovered. The underlying intermittent error remains unexplained. This
does not establish authenticated-history or credential upgrade acceptance. The
earlier public 0.1.0 → 0.1.1 upgrade also passed.

Download and extract the archive, then run `bash install.sh`. The per-user
installation includes Qt and needs no administrator privileges. This preview
targets Ubuntu 26.04 x86_64 / glibc 2.43 or newer. Other distributions and
operating systems remain unaccepted. The disk cache defaults to a configurable
decimal 10 GB. Keep an existing client available for unfinished workflows.

Historical intermittent channel-member selection and archive-dialog checks
remain under investigation; the current passing run does not establish their
causes or resolution.
