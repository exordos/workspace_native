import AppKit
import ApplicationServices
import Foundation

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
      let size = windowBounds(pid), let running = NSRunningApplication(processIdentifier: pid) else {
    throw fail("The package did not present its window.")
}
print("Observed packaged macOS process \(pid) with a visible window \(Int(size["Width"]!)) × \(Int(size["Height"]!)).")
guard running.hide(), waitFor(5, { windowBounds(pid) == nil }) else {
    throw fail("The primary process did not hide its window.")
}
let secondary = Process()
secondary.executableURL = executable
try secondary.run()
defer { if secondary.isRunning { secondary.terminate(); secondary.waitUntilExit() } }
guard waitFor(15, { !secondary.isRunning }), secondary.terminationStatus == 0,
      waitFor(10, { app.isRunning && windowBounds(pid) != nil }) else {
    throw fail("The second launch did not restore the original application.")
}
print("A second launch restored the original hidden application.")

guard AXIsProcessTrusted(), CGPreflightPostEventAccess() else {
    throw fail("The test runner lacks Accessibility or PostEvent permission for system clipboard input.")
}
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
func command(_ key: CGKeyCode, _ pid: pid_t) {
    for down in [true, false] {
        let event = CGEvent(keyboardEventSource: nil, virtualKey: key, keyDown: down)!
        event.flags = .maskCommand
        event.postToPid(pid)
    }
}
let a11y = AXUIElementCreateApplication(pid)
var field: AXUIElement?
guard waitFor(15, { field = addressField(a11y); return field != nil }),
      let field, AXUIElementSetAttributeValue(field, kAXFocusedAttribute as CFString, kCFBooleanTrue) == .success else {
    throw fail("The accessible login address field could not be focused.")
}
let pasteboard = NSPasteboard.general
let text = "https://cassi-package.invalid/clipboard"
pasteboard.clearContents()
pasteboard.setString(text, forType: .string)
command(0, pid) // Command+A
command(9, pid) // Command+V
guard waitFor(5, { attribute(field, kAXValueAttribute as CFString) as? String == text }) else {
    throw fail("The application did not paste the system clipboard.")
}
pasteboard.clearContents()
pasteboard.setString("cassi-copy-control", forType: .string)
command(0, pid)
command(8, pid) // Command+C
guard waitFor(5, { pasteboard.string(forType: .string) == text }) else {
    throw fail("The application did not copy the selected text.")
}
print("The native login field pasted and copied text through the macOS pasteboard.")
