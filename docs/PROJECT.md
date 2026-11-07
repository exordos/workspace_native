# Workspace Native

This repository contains the native C++17 / Qt Quick desktop client for Exordos
Workspace. Linux is the first acceptance target; Windows and macOS follow.
Development is in progress and complete desktop parity is not established.

Start with [build and runtime instructions](NATIVE_DESKTOP_DEVELOPMENT.md), the
[parity matrix](NATIVE_DESKTOP_PARITY.md), and [crypto acceptance](NATIVE_DESKTOP_CRYPTO.md).
The application cache is stored on disk with a configurable 10 GB default limit.

## Repository layout

`packages/native` owns the CMake project, C++ runtime, QML views and native tests.
The small `packages/web` and `packages/electron` trees contain pinned reference
assets, theme fixtures and compatibility source metadata. They do not run a web
application or Electron process. Their paths are retained so the accepted native
sources and resource paths remain unchanged during extraction.

CMake builds and the normal CTest suites use the checked-in bundled JavaScript
peer and require Node.js, without npm installation or an external UI checkout.
The optional peer-regeneration and independent cross-provider scripts still take
an explicit pinned upstream UI checkout/dependency directory; their inputs and
versions are recorded in `packages/native/tests/e2ee-peer/peer-provenance.json`.
They are development interoperability tools, not application runtime dependencies. Original test-peer package licenses are retained under `licenses/test-peer`.

## Imported state and history

The initial import preserves the accepted V34 native sources and tests. Its
original Linux checks passed Core246, UI124, Controller739, Window221, Cache83,
Service53, Runtime10, locale14 and Markdown21, plus installed runtime and anonymous
Wayland lifecycle checks. These earlier checks precede repository extraction;
fresh extraction checks are recorded in [extraction checks](provenance/extraction-checks.json). All build targets, eight input/runtime groups, two cache groups and the standalone Source-peer exchange passed in the independent checkout.

The imported snapshot and pinned source references are listed in
[provenance](provenance/import-v34.json). This repository starts its history with
the current snapshot; it does not reconstruct or claim earlier commits.
The next ordinary-channel manager Reject changes remain a separate work in progress.
Their whole Core suite has unresolved observations and is not part of this import.

Live service, complete Linux parity, full shell comparison, installers, signing,
Windows and macOS acceptance remain open.
