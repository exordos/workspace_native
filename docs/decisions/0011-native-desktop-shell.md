# Windows taskbar and macOS Dock ownership

**Status** Proposed in the Windows and macOS desktop acceptance changes.

**Context** The native desktop, notification service and updater already own unread state, attention requests and transfer progress respectively. Windows Explorer and the macOS Dock each need those values from an application running on that operating system.

**Options** Qt's notification-area component handles the tray images. The taskbar overlay, Dock tile and attention APIs require platform calls. Routing those calls through a separate process would require its own session and window ownership.

**Decision** Keep the existing feature owners and have them publish through a small platform-specific library in the application process. Use the visible main window for Windows taskbar updates. AppKit owns the application-wide Dock tile. The adapter reads the compiled reference images and receives the existing normalized progress values. Account data, credentials and release metadata stay with their original owners.

**Consequences** The native shell updates locally and needs no network connection. Clearing unread or progress restores the default presentation. A hosted acceptance window verifies native behavior on each platform. A second runner verifies that the extracted package can start. The existing updater can download on Linux x86_64. Supporting an installable update on Windows or macOS requires a platform release and installation contract.

[Packaging and acceptance](../NATIVE_DESKTOP_PLATFORM_PACKAGING.md)
