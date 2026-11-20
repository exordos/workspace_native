import AppKit
import ApplicationServices
import Foundation

setbuf(stdout, nil)
let pid = pid_t(CommandLine.arguments[1])!
let mode = CommandLine.arguments[2]
let origin = CommandLine.arguments[3]
let root = AXUIElementCreateApplication(pid)
func fail(_ message: String) -> NSError {
    NSError(domain: "WorkspaceAccountAcceptance", code: 1, userInfo: [NSLocalizedDescriptionKey: message])
}
func attr(_ item: AXUIElement, _ key: CFString) -> CFTypeRef? {
    var raw: CFTypeRef?
    return AXUIElementCopyAttributeValue(item, key, &raw) == .success ? raw : nil
}
func find(_ item: AXUIElement, _ name: String, _ role: String, _ depth: Int = 0) -> AXUIElement? {
    if depth > 24 { return nil }
    if attr(item, kAXRoleAttribute as CFString) as? String == role {
        let labels = [kAXTitleAttribute, kAXDescriptionAttribute, kAXPlaceholderValueAttribute]
            .compactMap { attr(item, $0 as CFString) as? String }
        let staticValue = role == kAXStaticTextRole as String ? attr(item, kAXValueAttribute as CFString) as? String : nil
        let titleElement = attr(item, kAXTitleUIElementAttribute as CFString)
        let linkedLabel: String? = titleElement.flatMap { value in
            guard CFGetTypeID(value) == AXUIElementGetTypeID() else { return nil }
            let label = value as! AXUIElement
            return (attr(label, kAXValueAttribute as CFString) ?? attr(label, kAXTitleAttribute as CFString)) as? String
        }
        if (labels.contains(name) || staticValue == name || linkedLabel == name), (attr(item, kAXEnabledAttribute as CFString) as? Bool) != false { return item }
    }
    for child in attr(item, kAXChildrenAttribute as CFString) as? [AXUIElement] ?? [] {
        if let match = find(child, name, role, depth + 1) { return match }
    }
    return nil
}
func control(_ name: String, _ role: String) throws -> AXUIElement {
    let deadline = Date().addingTimeInterval(15)
    repeat {
        if let item = find(root, name, role) { return item }
        RunLoop.current.run(until: Date().addingTimeInterval(0.1))
    } while Date() < deadline
    throw fail("The application did not expose an enabled \(name) control.")
}
func command(_ key: CGKeyCode) {
    for down in [true, false] {
        let event = CGEvent(keyboardEventSource: nil, virtualKey: key, keyDown: down)!
        event.flags = .maskCommand
        event.postToPid(pid)
    }
}
func fill(_ name: String, _ value: String) throws {
    let field = try control(name, kAXTextFieldRole as String)
    guard AXUIElementSetAttributeValue(field, kAXFocusedAttribute as CFString, kCFBooleanTrue) == .success else {
        throw fail("The \(name) field refused focus.")
    }
    if name == "Password" {
        // A secure text field can deliberately withhold its value. The server
        // accepts the sign-in only after the actual password arrives.
        for code: CGKeyCode in [35, 0, 8, 40, 0, 5, 14, 27, 2, 14, 46, 31] { // package-demo
            for down in [true, false] {
                CGEvent(keyboardEventSource: nil, virtualKey: code, keyDown: down)?.postToPid(pid)
            }
        }
        return
    }
    let board = NSPasteboard.general
    board.clearContents()
    board.setString(value, forType: .string)
    command(0)
    command(9)
    // Let Qt consume the paste before the next field changes the global pasteboard.
    let deadline = Date().addingTimeInterval(3)
    repeat {
        let content = attr(field, kAXValueAttribute as CFString) as? String ?? ""
        if content == value { return }
        RunLoop.current.run(until: Date().addingTimeInterval(0.1))
    } while Date() < deadline
    throw fail("The \(name) field did not receive its value.")
}
func press(_ name: String) throws {
    let button = try control(name, kAXButtonRole as String)
    guard AXUIElementPerformAction(button, kAXPressAction as CFString) == .success else {
        throw fail("The \(name) action failed.")
    }
}
guard AXIsProcessTrusted(), CGPreflightPostEventAccess() else { throw fail("The native desktop input permissions are unavailable.") }
switch mode {
case "login":
    try fill("Server address", origin)
    try press("Next")
    try fill("Email or login", "package-member")
    try fill("Password", "package-demo")
    try press("Next")
    try press("Login")
case "choose-file":
    try press("General Chat: Cassi hosted channel")
    try press("# Hosted call")
    guard let application = NSRunningApplication(processIdentifier: pid), application.activate(options: [.activateIgnoringOtherApps]) else {
        throw fail("Could not activate the signed-in application.")
    }
    try press("Attach files")
    _ = try control("Cancel", kAXButtonRole as String)
    let name = URL(fileURLWithPath: origin).lastPathComponent
    let stem = URL(fileURLWithPath: origin).deletingPathExtension().lastPathComponent
    func target(_ item: AXUIElement, _ depth: Int = 0) -> AXUIElement? {
        guard depth < 24 else { return nil }
        let labels = [kAXTitleAttribute, kAXDescriptionAttribute, kAXValueAttribute, kAXFilenameAttribute]
            .compactMap { attr(item, $0 as CFString) as? String }
        if labels.contains(where: { $0 == name || $0 == stem || $0.hasPrefix(name + ",") }) { return item }
        for child in attr(item, kAXChildrenAttribute as CFString) as? [AXUIElement] ?? [] {
            if let found = target(child, depth + 1) { return found }
        }
        return nil
    }
    for down in [true, false] {
        let desktop = CGEvent(keyboardEventSource: nil, virtualKey: 2, keyDown: down)!
        desktop.flags = [.maskCommand, .maskShift]
        desktop.post(tap: .cghidEventTap)
    }
    let timeout = Date().addingTimeInterval(12)
    var selected: AXUIElement?
    repeat {
        selected = target(root)
        if selected != nil { break }
        RunLoop.current.run(until: Date().addingTimeInterval(0.15))
    } while Date() < timeout
    guard let entry = selected else { throw fail("The AppKit chooser did not show the synthetic desktop file.") }
    guard let pos = attr(entry, kAXPositionAttribute as CFString), CFGetTypeID(pos) == AXValueGetTypeID(),
          let size = attr(entry, kAXSizeAttribute as CFString), CFGetTypeID(size) == AXValueGetTypeID() else {
        throw fail("The file entry did not expose its position.")
    }
    var originPoint = CGPoint.zero
    var extent = CGSize.zero
    guard AXValueGetValue(pos as! AXValue, .cgPoint, &originPoint),
          AXValueGetValue(size as! AXValue, .cgSize, &extent), extent.width > 0 && extent.height > 0 else {
        throw fail("The file entry did not expose visible bounds.")
    }
    let center = CGPoint(x: originPoint.x + extent.width / 2, y: originPoint.y + extent.height / 2)
    for eventType: CGEventType in [.leftMouseDown, .leftMouseUp] {
        let event = CGEvent(mouseEventSource: nil, mouseType: eventType, mouseCursorPosition: center, mouseButton: .left)!
        event.setIntegerValueField(.mouseEventClickState, value: 1)
        event.post(tap: .cghidEventTap)
    }
    try press("Open")
    print("Selected the synthetic Desktop file and pressed Open in its native panel.")
case "join-call":
    try press("General Chat: Cassi hosted channel")
    try press("Join call")
case "close-call":
    try press("Close call")
case "update":
    try press("Profile")
    try press("App version")
    try press("Check for updates")
    _ = try control("Updates are not available for this platform.", kAXStaticTextRole as String)
    try press("Close")
case "logout":
    try press("Profile")
    try press("Sign out")
    _ = try control("Server address", kAXTextFieldRole as String)
case "quit":
    _ = NSRunningApplication(processIdentifier: pid)?.activate(options: [.activateIgnoringOtherApps])
    command(12) // Q
default: throw fail("Unknown account test mode.")
}
