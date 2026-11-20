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

The Mac bundle links QtSvg so macdeployqt includes the SVG image reader used by QML. The clean launch checks that the extracted application loads that reader from its own bundle and reports any embedded startup icon that failed to decode. [Qt macOS plugin deployment](https://doc.qt.io/qt-6/macos-deployment.html)

CMake installs the application using Qt's QML deployment script. The package jobs build the pinned Markdown and keychain libraries against the same Qt kit. OpenSSL supplies the existing native TLS and HPKE APIs. The Windows and macOS smoke checks run independently so a failure on either platform remains visible.

The package targets follow the [Qt QML deployment documentation](https://doc.qt.io/qt-6/qt-generate-deploy-qml-app-script.html).

## Clean-desktop interaction

Each platform test minimizes the existing window and launches the unpacked executable a second time. The second process exits after the original process acknowledges activation. The test observes the original window restored, focuses the accessible Server address field, pastes text through the real OS clipboard and copies a new selection back. Windows uses UI Automation and Ctrl+V/C. macOS uses the Accessibility API and Command+V/C. The selection includes Cyrillic and spaces and cannot be submitted as a server address.

| Capability | Windows x64 | macOS arm64 |
| --- | --- | --- |
| Start in a relocated archive, restore the original window, clipboard text in both directions | PASS on Windows Server 2022 | PASS on macOS 15 |
| Sign in, quit, restore the rotated account in a second process and sign out | PASS, including Windows Credential Manager entry creation and removal | PASS, including macOS Keychain entry creation and removal |
| Check for updates in App version | The Windows preview reports that updates are unavailable | The macOS preview reports that updates are unavailable |
| Attach a local file through the native file chooser | PASS, the Windows common dialog returned the selected file | PASS, the AppKit Open panel returned the selected file |
| OS delivery of a registered URL | NOT RUN | PASS, Launch Services opened the requested message in a running application and after a full quit and account restore |
| File and image clipboard formats | NOT RUN | NOT RUN |
| Embedded meeting loads and asks for microphone and camera | Hosted Windows reported zero capture endpoints and the meeting returned `NotFoundError` for both devices | AVFoundation identified two audio endpoints and no video endpoint. The meeting supplied a live audio track and returned `NotFoundError` for video |
| Native screen reader narration and display scaling | Manual Narrator and display-scale checks pending | Manual VoiceOver and display-scale checks pending |
| Signed installer or notarized distribution and update/relaunch | Requires the Windows distribution channel and signing identity | Requires the macOS distribution channel and signing/notarization identity |

The account check uses a disposable loopback HTTPS organization. The normal installed executable performs password login, rotates its project-scoped refresh credential, and signs in again after restart. The test checks whether its randomly identified entry exists. It never inspects, decodes or logs the credential value. On Windows it frees the structure returned by Credential Manager immediately. Signing out must return to the login screen and remove that account. The runner removes the temporary certificate and account data before disposal.

On each desktop, the App version panel displayed the unavailable update status after Check for updates. The Windows screenshot and macOS WindowServer capture showed the message on screen. The previews have no Windows or macOS update installation path.

On both desktops, the signed-in application opened **Attach files** in a writable topic. The fixture received one authenticated upload with the exact filename and bytes chosen in the native dialog. [Windows file selection](https://github.com/exordos/workspace_native/actions/runs/44837008062/job/136635857672) · [macOS file selection](https://github.com/exordos/workspace_native/actions/runs/44838282721/job/136640055302)

On Apple Silicon, the warm and cold URL checks requested different messages. The cold target was outside the first page of restored history. The native accessibility tree showed the requested text in each case. [Launch Services acceptance](https://github.com/exordos/workspace_native/actions/runs/44837259081)

For the call check, the package joins a meeting served by the same loopback HTTPS fixture. The meeting calls `getUserMedia`, reads each track's state and stops the track without retaining media. Windows Runtime and AVFoundation count the devices visible to that runner independently. The package must return a live track for an available device or report that a device is absent. A fresh permission prompt and a physical camera require a desktop with the corresponding OS privacy state and hardware. The current Qt WebEngine permission interface omits the requesting frame identity, so the call controller declines display capture before the platform picker. [Display-capture policy](NATIVE_DESKTOP_CALL_DISPLAY_CAPTURE.md)

The native accessibility APIs establish that the login field can be focused and read. Spoken narration and other application screens have their own acceptance criteria. Live authentication, Workspace data synchronization, media permissions and cross-client calls are tracked in the [desktop acceptance plan](NATIVE_DESKTOP_PARITY.md#acceptance-plan).

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

## Windows per-user installer

Each Windows package includes a portable ZIP and a per-user MSI built by CPack and WiX from the same staged application. Check the chosen file against `SHA256SUMS`. The installer places the application under `%LOCALAPPDATA%\Workspace Native Preview` and creates a shortcut in the current user's Start menu. The shortcut and running application use `com.exordos.workspace.native.preview` as their Windows application ID. Users can remove the preview from Windows Settings under Installed apps.

A two-version installation test removes an obsolete file during the MSI upgrade and retains an unrelated file. The clean Windows runner checks the installer digest, installs into a fresh profile, reads the current user's uninstall registration and Start menu shortcut, and launches the installed executable. After uninstall it checks that the application's files and registration are gone and a file it created alongside them was preserved.
