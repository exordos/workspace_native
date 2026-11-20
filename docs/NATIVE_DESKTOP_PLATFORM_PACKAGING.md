# Windows and macOS desktop packages

Build the existing native CMake application in Release mode with Qt 6.10.2 on Windows x64, Intel macOS and Apple Silicon macOS. On Windows the portable ZIP keeps the executable in `bin` with the Qt plugins and QML imports under the installation prefix. On macOS the ZIP preserves the application bundle, symlinks and executable permissions. Both archives include the bundled license notices.

The repository workflow builds each platform on its own GitHub-hosted runner and stores the archive and SHA-256 digest as workflow artifacts. A fresh runner downloads that platform's artifacts, verifies the digest, extracts the archive into a path with spaces and launches the packaged executable with its native desktop backend. It verifies a visible, nonempty window owned by that process. The macOS launch job also checks the runner architecture and verifies that the application executable contains the matching Mach-O slice. The smoke runner installs no Qt SDK or build dependencies.

| Acceptance | Windows x64 | macOS (Apple Silicon and Intel) |
| --- | --- | --- |
| Package integrity, relocated application, first visible window | Hosted Windows window enumeration | Hosted macOS WindowServer enumeration |
| Normal and unread tray assets | Windows notification-area icon | macOS status item |
| Unread and attention | Shell taskbar unread dot and accessible name | Dock unread icon, active-call badge and informational bounce |
| Updater progress | Taskbar progress and its cleared state | Dock progress and its cleared state |

Clean runners verified all three archive digests and observed a visible window owned by each extracted application. Windows x64 reported a 1024 × 728 client area, Apple Silicon reported a 1024 × 681 window and Intel macOS reported a 1440 × 928 window. The Intel launch also verified the x86_64 executable and its ad hoc signature. The first Windows build caught a Windows SDK macro expanding the unread-count saturation expression in the application entry point. The same source now compiles with MSVC and launches from the relocated ZIP.

The package workflow runs the shell checks on the build runners. Fresh launch runners then validate the relocated archives. The shell checks observe controlled unread and progress events on each operating system. The existing Linux workflow and its Release acceptance results remain independent. The workflow produces an unsigned Windows preview and an ad hoc signed macOS preview. Windows signing, Apple notarization and store distribution require their own release process.

The archives carry the Qt SQLite driver used by the cache, diagnostics and key vault. The packaging step removes other SQL drivers because optional SDK plugins depend on host-only database libraries. The Windows portable ZIP includes the release-only MSVC runtime alongside its executable. The native launch check examines its loaded modules and confirms that the process uses the copies inside the relocated archive. CMake selects these files from the build toolchain’s redistributable directory. [CMake runtime installation](https://cmake.org/cmake/help/latest/module/InstallRequiredSystemLibraries.html)

CMake installs the application using Qt's QML deployment script. The package jobs build the pinned Markdown and keychain libraries against the same Qt kit. OpenSSL supplies the existing native TLS and HPKE APIs. The Windows and macOS smoke checks run independently so a failure on either platform remains visible.

The package targets follow the [Qt QML deployment documentation](https://doc.qt.io/qt-6/qt-generate-deploy-qml-app-script.html).

## Native shell acceptance

The Electron reference keeps the macOS Dock text empty for unread messages and selects the normal or dotted Dock image. During a call it keeps the normal icon and shows a telephone badge. Ending the call restores the current unread image. The application-wide numeric badge call in the reference runs on Linux. The macOS check reads the actual AppKit icon pixels and Dock badge after each transition and compares both images with the reference PNGs.

On Windows the overlay is a transparent 16 × 16 icon containing an orange circle with a two-pixel radius. Its accessible description follows the current application language. The Windows acceptance window checks for added and removed orange pixels on Explorer's taskbar. A separate Windows check compares the notification area and its overflow panel as the tray icon changes to unread and back. On macOS the check reads the image installed on the native status item.

Windows associates an overlay and progress with a particular taskbar button.
Explorer announces the initial button and its replacement through
`TaskbarButtonCreated`. The native window retains its latest unread and
progress values so the replacement can display them without another
notification or download event. [Microsoft’s taskbar interface](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nn-shobjidl_core-itaskbarlist3)

The shell adapter accepts download progress from the existing updater bridge. The macOS check reads the native progress indicator on the Dock tile. The Windows check captures Explorer's taskbar while the progress bar appears and clears. The current release updater supports Linux x86_64 and reports that updates are unavailable on other platforms. The Windows and macOS checks therefore supply controlled progress values to the shell adapter. A signed Windows or macOS update channel requires its own distribution and installation acceptance.

[Desktop shell ownership decision](decisions/0011-native-desktop-shell.md)
