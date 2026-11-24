# Exordos Workspace Native

Exordos Workspace Native is the native desktop client for Exordos Workspace, the
team messaging and collaboration platform. Built with Qt 6 and QML, it is a real
desktop application: a single self-contained Linux archive, no browser tab, no
server to install alongside it.

## What you can do

- Sign in to your Exordos Workspace and move between projects from one window.
- Browse streams, topics and direct chats in the sidebar, with folders,
  unread counters and latest-message previews.
- Write messages with rich formatting, emoji and mentions; edit or delete them
  in place.
- Reply to and quote earlier messages, jump to the quoted original, and forward
  a selection to other conversations.
- Share files and images, and view them full-screen in the built-in media
  viewer.
- Save drafts and finish them later, in the same or another conversation.
- Hold end-to-end encrypted conversations, with keys kept on your device.
- Join workspace calls without leaving the app.
- Keep up with desktop notifications, unread markers, avatars and live presence.
- Search people to start a direct chat and look through the message history
  already cached on this machine.
- Pick a light or dark appearance, and keep recent history available offline.

## Getting started

Download the latest Linux archive (`Exordos-Workspace-Native-<version>-linux-x86_64.tar.gz`,
with `SHA256SUMS`) from the
[releases page](https://github.com/exordos/workspace_native/releases/latest),
unpack it and start `bin/workspace-native`. The archive carries its own runtime,
so nothing else needs to be installed.

## Local development

The [development guide](docs/NATIVE_DESKTOP_DEVELOPMENT.md) covers the build
requirements, building and running from source, and how the test suites are
organised and run. The current state of the client and what is being worked on
is described in [CURRENT_DEVELOPMENT.md](docs/CURRENT_DEVELOPMENT.md).

## Contributing

Product and design decisions, coding rules and documentation conventions live in
[AGENTS.md](AGENTS.md) and the [docs/](docs/) directory - read them before
opening a change. Bugs, feature requests and parity gaps are tracked in the
[issue tracker](https://github.com/exordos/workspace_native/issues); the
[parity ledger](docs/NATIVE_DESKTOP_PARITY.md) records how the client maps to
the reference desktop app, surface by surface.

## License

Licensed under the [Apache License, Version 2.0](LICENSE).
