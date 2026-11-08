# Workspace Native — Linux development preview

This is an early Qt native development build. Complete desktop feature and
pixel parity is still in progress. Linux is the current priority.

The downloadable archive contains the Qt runtime and a per-user installer.
Extract it and run `bash install.sh`; start Workspace Native Preview from the
application menu or `~/.local/bin/workspace-native`. No administrator privileges
are required. This initial build targets Ubuntu 26.04 x86_64 / glibc 2.43 or newer;
other distributions and operating systems have not yet been accepted.

Use **Check for updates** in the tray menu to download, verify and install a newer
GitHub release, then restart. A new binary version is installed beside the old
one. Existing settings, secure-store credentials and disk cache are separate.
The disk cache defaults to a configurable decimal 10 GB.

The native client includes ordinary/encrypted conversation foundations, channel
and topic navigation, composer and attachments, reactions, cache, profile/status
presentation and tray behavior. Several desktop sections and live acceptance
remain unfinished; keep an existing client available for those workflows.
