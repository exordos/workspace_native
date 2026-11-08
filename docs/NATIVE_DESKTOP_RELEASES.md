# Linux builds and updates

Sources, build artifacts and releases belong to this public repository,
`exordos/workspace_native`. The default branch is `master`. There is no separate
binary release feed. The initial pipeline and updater are under development;
the [initial checks](provenance/linux-updater-initial-checks.json) explicitly
retain the first whole-window failure and outstanding release acceptance.

## Build pipeline

`.github/workflows/linux-release.yml` builds on master pushes, `vX.Y.Z` tags
and manual dispatch. It uses an Ubuntu 26.04 container on an Ubuntu 24.04 runner,
installs the documented Qt kit, checks installer/package regressions, builds
Release, deploys the actual QML runtime and uploads the archive and checksums.
Tag runs also publish those files in this repository's GitHub Releases using
the workflow's repository-scoped token. Application packages contain no token.

The tag, configured CMake version and source project version must agree.
`main.cpp` receives the version from CMake. A source version bump cannot relabel
an older configured build. Tags are published after the corresponding local
checks and installed runtime/upgrade acceptance. An artifact upload alone is
not release or installed-app acceptance.

## Local package

Configure a Linux x86_64 Release build with `CMAKE_INSTALL_LIBDIR=lib`, then run:

```sh
python3 packages/native/scripts/package-linux.py \
  --build-dir /absolute/path/to/release-build \
  --output-dir /absolute/path/to/new-package-directory --version 0.1.0
```

The script installs into a fresh prefix, verifies deployed dependencies, collects
actual ELF package origins and copyright/license notices, and writes a `.tar.gz`
and `SHA256SUMS`. RPATH-adjusted libraries are matched by unchanged ELF build ID,
class, architecture and SONAME when their full hash differs. Source package
versions and Ubuntu source locators accompany the notices; the collector does
not download or independently verify those source URLs.

This first package targets Ubuntu 26.04 x86_64 / glibc 2.43 or newer. Distribution,
architecture and operating-system coverage must be expanded and measured
separately. System graphics drivers, fonts, the desktop session and credential
service remain host dependencies.

## Install and update

Verify the downloaded archive against `SHA256SUMS`, extract it, and run
`bash install.sh` inside its top-level directory. The installer uses
`${XDG_DATA_HOME:-$HOME/.local/share}/workspace-native-install/versions/X.Y.Z`,
switches `current` after preparation and registers a per-user launcher and
desktop entry. It does not request root privileges or remove account/cache data.
The stable launcher is `~/.local/bin/workspace-native`; a previous binary version
remains available. Direct execution of the extracted `bin/workspace-native`
also remains possible.

The updater has an independent unauthenticated network manager. It checks this
repository's latest GitHub release ten seconds after Linux startup, every four
hours, or through the tray action. It accepts a newer strict numeric version and
the matching x86_64 asset, streams it with fixed memory/size/time bounds and
checks the GitHub asset's SHA-256 before installation. Requests carry no Workspace
cookies or credentials. Failed/cancelled downloads remove their own temporary
files. Installer cancellation first allows shell cleanup before a bounded hard
stop. Restart waits for cache/key shutdown and releases the primary-instance
lock before starting the selected binary.

The current update dialog is a functional initial view. Source's About card and
toolbar indicator, full visual equivalence, actual installed upgrade, GitHub
asset delivery and complete live application acceptance remain separate checks.
