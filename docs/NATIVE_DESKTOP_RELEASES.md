# Linux builds and updates

Sources, build artifacts and releases belong to this public repository,
`exordos/workspace_native`. The default branch is `master`. There is no separate
binary release feed. The initial pipeline and updater are under development;
the [initial checks](provenance/linux-updater-initial-checks.json) explicitly
retain the first whole-window failure and outstanding release acceptance.

## Published preview

[0.1.0](https://github.com/exordos/workspace_native/releases/tag/0.1.0) is the
first public Linux preview. Its archive and SHA256SUMS are available in that
release. The actual downloaded archive passed checksum, per-user installation,
anonymous Wayland startup, private runtime mapping, five-codec/crypto/TLS and
explicit quit checks on the current Ubuntu host. Full functionality, visual
parity remain open. The initial
installed public update check failed; a later check on the unchanged downloaded
binary succeeded. Its earlier failure remains unexplained.
See [the recorded checks](provenance/linux-first-published-preview-checks.json).

[0.1.1](https://github.com/exordos/workspace_native/releases/tag/0.1.1) adds the
About dock and automatic download. Both CI runs and its actual public archive
passed their recorded checks. The actual installed 0.1.0 → 0.1.1 download,
install and restart passed, including same-primary activation and preservation
of one synthetic NativeCache History row. Authenticated account/history and
secure-store preservation remain separate acceptance. Intermittent public check
errors remain unexplained. See [published upgrade checks](provenance/linux-published-0.1.1-upgrade-checks.json).

[0.1.3](https://github.com/exordos/workspace_native/releases/tag/0.1.3) adds bounded
static received-image previews. Both CI runs and the actual public archive passed
their checks. The installed 0.1.2 → 0.1.3 automatic download, one install action,
restart, same-primary activation and normal quit passed. One synthetic NativeCache
History row was preserved and reopened. The new version's metadata check succeeded
without a manual retry; earlier intermittent failures remain unexplained.
Authenticated account/history, credential preservation and full application parity
remain separate. See [published 0.1.3 upgrade checks](provenance/linux-published-0.1.3-upgrade-checks.json).

[0.1.4](https://github.com/exordos/workspace_native/releases/tag/0.1.4) repairs
a picked image missing after Send when the draft already contains a caption.
Tag/master CI and the actual downloaded archive passed their checks. The
completed public 0.1.3 → 0.1.4 update passed with one Install and restart action,
normal old/new exits, exact binary/current link, same-primary activation and
one synthetic NativeCache History row preserved/reopened. No manual metadata
recovery was needed in that completed run. A separate interrupted run retained
one real manual Check recovery; its interruption and metadata-error causes
remain unknown. Portal/AT-SPI diagnostics remain. Live-account, credential
preservation and full application/Linux parity stay outside this acceptance.
See [published 0.1.4 update checks](provenance/linux-published-0.1.4-upgrade-checks.json).

## Build pipeline

`.github/workflows/linux-release.yml` builds on master pushes, plain `X.Y.Z` tags
(for example `0.1.0`, without a prefix) and manual dispatch. It uses an Ubuntu 26.04 container on an Ubuntu 24.04 runner,
installs the documented Qt kit and Node.js for test-target configuration, checks
installer/package regressions, builds
Release, deploys the actual QML runtime, exercises the packaged five-codec
(including SVG), encryption and TLS probe, and uploads the archive and checksums.
Tag runs also publish those files in this repository's GitHub Releases using
the workflow's repository-scoped token. Application packages contain no token.

The tag, configured CMake version and source project version must agree.
`main.cpp` receives the version from CMake. A source version bump cannot relabel
an older configured build. The first development-preview tag follows its local
checks and installed-runtime acceptance. As requested, the first actual upgrade
between released versions is verified after that initial publication. An artifact upload alone is
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

### Metadata check recovery (0.1.5)

The development candidate permits one automatic metadata GET when an HTTP/2
connection fails before any response status, headers or JSON bytes arrive. It
requires the actual negotiated `h2` protocol and Qt's `ProtocolFailure` error.
The same logical Check stays in `checking`, keeps its original 30-second total
deadline and uses the same endpoint and request policy. A second eligible failure
is terminal. HTTP status errors, TLS errors, invalid release metadata, and errors
after response headers do not use this recovery. Download and redirect rules are
unchanged; HTTP/1 is not forced.

The owned local TLS/h2 regression failed before this change and recovered after
it without a third manual Check. Eighteen independent transport/lifecycle controls
also passed using the published 0.1.4 Qt/OpenSSL runtime. These establish the
bounded recovery behavior, not the cause of earlier intermittent public errors.
The complete existing updater suite passed 49 cases. The local package/runtime probe and checkpoint GitHub packaging CI also passed.
Public tag/assets and the installed upgrade remain pending. See the
[sanitized recovery checks](provenance/updater-http2-recovery-checks.json).

The Source-shaped About dock and toolbar indicator share the app-scoped updater.
Available releases download automatically. The verified download exposes one
explicit Install and restart action; disclosure never starts or cancels work.
The actual Main and lifecycle policy checks are recorded in
[About/update checks](provenance/about-update-checks.json). CI runs the policy
object graph without a native surface; that is not a visual/window check.
Full visual equivalence, licenses navigation and complete live application
acceptance remain separate. Actual public asset delivery and anonymous installed
upgrade are recorded above; they do not establish all account/session workflows.
