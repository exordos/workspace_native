# Windows and macOS desktop packages

Build the existing native CMake application in Release mode with Qt 6.10.2 on Windows x64, Intel macOS and Apple Silicon macOS. On Windows the portable ZIP keeps the executable in `bin` with the Qt plugins and QML imports under the installation prefix. On macOS the ZIP preserves the application bundle, symlinks and executable permissions. Both archives include the bundled license notices.

The repository workflow builds each platform on its own GitHub-hosted runner and stores the archive and SHA-256 digest as workflow artifacts. A fresh runner downloads that platform's artifacts, verifies the digest, extracts the archive into a path with spaces and launches the packaged executable with its native desktop backend. It verifies a visible, nonempty window owned by that process. The macOS launch job also checks the runner architecture and verifies that the application executable contains the matching Mach-O slice. The smoke runner installs no Qt SDK or build dependencies.

| Acceptance | Windows x64 | macOS arm64 |
| --- | --- | --- |
| Package integrity, relocated application, first visible window | Hosted Windows window enumeration | Hosted macOS WindowServer enumeration |
| Normal and unread tray assets | Windows notification-area icon | macOS status item |
| Unread and attention | Shell taskbar unread dot and accessible name | Dock badge and attention bounce |
| Updater progress | Taskbar progress and its cleared state | Dock progress and its cleared state |

Clean runners verified both archive digests and observed a visible window owned by each extracted application. Windows x64 reported a 1024 × 728 client area and macOS arm64 reported a 1024 × 681 window. The first Windows build caught a Windows SDK macro expanding the unread-count saturation expression in the application entry point. The same source now compiles with MSVC and launches from the relocated ZIP.

The first row is automated by the package workflow. The OS integration rows need an observed shell state and a controlled unread or updater event. A successful package launch alone cannot certify those rows. The existing Linux workflow and its Release acceptance results remain independent. The workflow produces an unsigned Windows preview and an ad hoc signed macOS preview. Windows signing, Apple notarization and store distribution require their own release process.

Both archives carry the Qt SQLite driver used by the cache, diagnostics and key vault. The packaging step removes other SQL drivers because optional SDK plugins depend on host-only database libraries. The Windows portable ZIP includes the release-only MSVC runtime alongside its executable. The native launch check examines its loaded modules and confirms that the process uses the copies inside the relocated archive. CMake selects these files from the build toolchain’s redistributable directory. [CMake runtime installation](https://cmake.org/cmake/help/latest/module/InstallRequiredSystemLibraries.html)

CMake installs the application using Qt's QML deployment script. The package jobs build the pinned Markdown and keychain libraries against the same Qt kit. OpenSSL supplies the existing native TLS and HPKE APIs. The Windows and macOS smoke checks run independently so a failure on either platform remains visible.

The package targets follow the [Qt QML deployment documentation](https://doc.qt.io/qt-6/qt-generate-deploy-qml-app-script.html).
