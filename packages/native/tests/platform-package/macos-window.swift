import AppKit
import ApplicationServices
import CoreServices
import Foundation

setbuf(stdout, nil)

func fail(_ message: String) -> NSError {
    NSError(domain: "WorkspacePackageSmoke", code: 1, userInfo: [NSLocalizedDescriptionKey: message])
}

func waitFor(_ seconds: TimeInterval, _ operation: () -> Bool) -> Bool {
    let deadline = Date().addingTimeInterval(seconds)
    repeat {
        if operation() { return true }
        RunLoop.current.run(until: Date().addingTimeInterval(0.1))
    } while Date() < deadline
    return operation()
}

func windowBounds(_ pid: Int32) -> [String: Double]? {
    let entries = CGWindowListCopyWindowInfo([.optionOnScreenOnly, .excludeDesktopElements], kCGNullWindowID) as? [[String: Any]] ?? []
    for entry in entries {
        guard let owner = entry[kCGWindowOwnerPID as String] as? Int, owner == Int(pid),
              entry[kCGWindowLayer as String] as? Int == 0,
              let bounds = entry[kCGWindowBounds as String] as? [String: Double],
              let width = bounds["Width"], let height = bounds["Height"], width >= 400, height >= 300 else { continue }
        return bounds
    }
    return nil
}

let executable = URL(fileURLWithPath: CommandLine.arguments[1])
let app = Process()
app.executableURL = executable
try app.run()
defer {
    if app.isRunning {
        app.terminate()
        app.waitUntilExit()
    }
}
let pid = app.processIdentifier
guard waitFor(90, { !app.isRunning || windowBounds(pid) != nil }), app.isRunning,
      let size = windowBounds(pid) else {
    throw fail("The package did not present its window.")
}
print("Observed packaged macOS process \(pid) with a visible window \(Int(size["Width"]!)) × \(Int(size["Height"]!)).")
let a11y = AXUIElementCreateApplication(pid)
print("Hosted desktop accessibility=\(AXIsProcessTrusted()) key events=\(CGPreflightPostEventAccess())")
guard AXIsProcessTrusted(), CGPreflightPostEventAccess() else {
    throw fail("The test runner lacks Accessibility or PostEvent permission for window and clipboard input.")
}
// WindowServer can expose the new surface before Qt publishes its AX window.
var primaryWindow: AXUIElement?
guard waitFor(15, {
    var value: CFTypeRef?
    guard AXUIElementCopyAttributeValue(a11y, kAXWindowsAttribute as CFString, &value) == .success,
          let windows = value as? [AXUIElement] else { return false }
    primaryWindow = windows.first { window in
        var settable = DarwinBoolean(false)
        return AXUIElementIsAttributeSettable(window, kAXMinimizedAttribute as CFString, &settable) == .success && settable.boolValue
    }
    return primaryWindow != nil
}), let primary = primaryWindow else {
    throw fail("Qt did not expose a minimizable accessible window.")
}
let minimized = AXUIElementSetAttributeValue(primary, kAXMinimizedAttribute as CFString, kCFBooleanTrue)
guard minimized == .success else { throw fail("The primary window refused to minimize: " + String(minimized.rawValue)) }
guard waitFor(10, { windowBounds(pid) == nil }) else { throw fail("The minimized primary window remained on the desktop.") }
let secondary = Process()
secondary.executableURL = executable
try secondary.run()
defer { if secondary.isRunning { secondary.terminate(); secondary.waitUntilExit() } }
guard waitFor(15, { !secondary.isRunning }), secondary.terminationStatus == 0,
      waitFor(10, { app.isRunning && windowBounds(pid) != nil }) else {
    throw fail("The second launch did not restore the original application.")
}
print("A second launch restored the original minimized window.")

let bundleURL = executable.deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
let bundle = Bundle(url: bundleURL)
let types = bundle?.object(forInfoDictionaryKey: "CFBundleURLTypes") as? [[String: Any]] ?? []
guard bundle?.object(forInfoDictionaryKey: "CFBundleDevelopmentRegion") as? String == "en",
      bundle?.object(forInfoDictionaryKey: "CFBundleAllowMixedLocalizations") as? Bool == true,
      bundle?.object(forInfoDictionaryKey: "NSPrincipalClass") as? String == "NSApplication",
      bundle?.object(forInfoDictionaryKey: "NSSupportsAutomaticGraphicsSwitching") as? Bool == true,
      bundle?.object(forInfoDictionaryKey: "LSMinimumSystemVersion") is String else {
    throw fail("The package lost its Qt macOS bundle metadata.")
}
let declaresWorkspaceLinks = types.contains { ($0["CFBundleURLSchemes"] as? [String])?.contains("ew") == true }
guard declaresWorkspaceLinks, LSRegisterURL(bundleURL as CFURL, true) == noErr else {
    throw fail("The installed bundle did not register as a Workspace URL handler.")
}
for key in ["NSCameraUsageDescription", "NSMicrophoneUsageDescription"] {
    guard let purpose = bundle?.object(forInfoDictionaryKey: key) as? String, !purpose.isEmpty else {
        throw fail("Missing media permission text for " + key)
    }
}
let workspaceLink = "ew://open/org/example/project/11111111-1111-4111-8111-111111111111/inbox"
guard AXUIElementSetAttributeValue(primary, kAXMinimizedAttribute as CFString, kCFBooleanTrue) == .success,
      waitFor(5, { windowBounds(pid) == nil }) else { throw fail("The window did not minimize before opening the URL.") }
let openURL = Process()
openURL.executableURL = URL(fileURLWithPath: "/usr/bin/open")
openURL.arguments = ["-a", bundleURL.path, workspaceLink]
try openURL.run()
openURL.waitUntilExit()
guard openURL.terminationStatus == 0, waitFor(10, { app.isRunning && windowBounds(pid) != nil }) else {
    throw fail("Opening the Workspace URL did not restore the original packaged window.")
}
print("Launch Services delivered the Workspace URL to the original minimized application.")

func attribute(_ element: AXUIElement, _ name: CFString) -> CFTypeRef? {
    var value: CFTypeRef?
    return AXUIElementCopyAttributeValue(element, name, &value) == .success ? value : nil
}
func addressField(_ element: AXUIElement, depth: Int = 0) -> AXUIElement? {
    if depth > 24 { return nil }
    if attribute(element, kAXRoleAttribute as CFString) as? String == kAXTextFieldRole as String {
        let labels = [kAXTitleAttribute, kAXDescriptionAttribute, kAXPlaceholderValueAttribute]
            .compactMap { attribute(element, $0 as CFString) as? String }
        if labels.contains("Server address") || labels.contains("https://chat.example.com") { return element }
    }
    let children = attribute(element, kAXChildrenAttribute as CFString) as? [AXUIElement] ?? []
    for child in children { if let match = addressField(child, depth: depth + 1) { return match } }
    return nil
}
func command(_ key: CGKeyCode) {
    let commandKey: CGKeyCode = 55
    let strokes: [(CGKeyCode, Bool, CGEventFlags)] = [
        (commandKey, true, .maskCommand), (key, true, .maskCommand),
        (key, false, .maskCommand), (commandKey, false, [])]
    for (code, down, flags) in strokes {
        let event = CGEvent(keyboardEventSource: nil, virtualKey: code, keyDown: down)!
        event.flags = flags
        event.post(tap: .cghidEventTap)
    }
}
guard let running = NSRunningApplication(processIdentifier: pid) else { throw fail("The primary application exited.") }
_ = running.activate(options: [.activateIgnoringOtherApps])
guard waitFor(10, { running.isActive }) else { throw fail("The primary window did not regain keyboard activation.") }
var field: AXUIElement?
guard waitFor(15, { field = addressField(a11y); return field != nil }),
      let field, AXUIElementSetAttributeValue(field, kAXFocusedAttribute as CFString, kCFBooleanTrue) == .success else {
    throw fail("The accessible login address field could not be focused.")
}
guard waitFor(5, {
    guard let focused = attribute(a11y, kAXFocusedUIElementAttribute as CFString) else { return false }
    return CFEqual(focused, field)
}) else { throw fail("The login address field did not regain actual keyboard focus.") }
let pasteboard = NSPasteboard.general
let text = "Cassi clipboard | Привет"
pasteboard.clearContents()
pasteboard.setString(text, forType: .string)
command(0) // Command+A
command(9) // Command+V
guard waitFor(5, { attribute(field, kAXValueAttribute as CFString) as? String == text }) else {
    throw fail("The application did not paste the system clipboard.")
}
pasteboard.clearContents()
pasteboard.setString("cassi-copy-control", forType: .string)
command(0)
command(8) // Command+C
guard waitFor(5, { pasteboard.string(forType: .string) == text }) else {
    throw fail("The application did not copy the selected text.")
}
print("The native login field pasted and copied text through the macOS pasteboard.")
