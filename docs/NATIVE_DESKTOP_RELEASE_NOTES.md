# Workspace Native — Linux development preview

This is an early Qt native development build. Complete desktop feature and
pixel parity is still in progress. Linux is the current priority.

The downloadable archive contains the Qt runtime and a per-user installer.
Extract it and run `bash install.sh`; start Workspace Native Preview from the
application menu or `~/.local/bin/workspace-native`. No administrator privileges
are required. This initial build targets Ubuntu 26.04 x86_64 / glibc 2.43 or newer;
other distributions and operating systems have not yet been accepted.

The tray includes **Check for updates** for subsequent releases. The initial
release is published before the first real two-release upgrade check; that
end-to-end update/restart acceptance follows. Binary versions are installed side
by side; settings, secure-store credentials and disk cache have separate paths.
The disk cache defaults to a configurable decimal 10 GB.

The native client includes ordinary/encrypted conversation foundations, channel
and topic navigation, composer and attachments, reactions, cache, profile/status
presentation and tray behavior. Several desktop sections and live acceptance
remain unfinished; keep an existing client available for those workflows.

Known UI checks still under investigation: intermittent channel-member selection
and archive-dialog closing failures. These are retained as open failures; this
preview does not establish complete Linux or live-account acceptance.
