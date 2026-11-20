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
        let secure = name == "Password" && attr(item, kAXSubroleAttribute as CFString) as? String == kAXSecureTextFieldSubrole as String
        if (labels.contains(name) || staticValue == name || secure), (attr(item, kAXEnabledAttribute as CFString) as? Bool) != false { return item }
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
    func report(_ item: AXUIElement, _ depth: Int) {
        if depth > 24 { return }
        let role = attr(item, kAXRoleAttribute as CFString) as? String ?? "unknown"
        let labels = [kAXTitleAttribute, kAXDescriptionAttribute, kAXPlaceholderValueAttribute, kAXValueAttribute]
            .compactMap { attr(item, $0 as CFString) as? String }
        let fixed = ["Server address", "Email or login", "Password", "Next", "Login", "Profile", "Sign out",
                     "Could not load organization settings. Check the server address and try again."]
        let subrole = attr(item, kAXSubroleAttribute as CFString) as? String ?? ""
        if let label = fixed.first(where: labels.contains) { print("Exposed \(role) \(label)") }
        if role.contains("Text") || role.contains("Edit") || !subrole.isEmpty {
            print("Field metadata role=\(role) subrole=\(subrole) id=\(attr(item, kAXIdentifierAttribute as CFString) as? String ?? "") enabled=\(attr(item, kAXEnabledAttribute as CFString) as? Bool ?? false)")
        }
        for child in attr(item, kAXChildrenAttribute as CFString) as? [AXUIElement] ?? [] { report(child, depth + 1) }
    }
    report(root, 0)
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
    if name == "Password" {
        // The previous field still holds actual keyboard focus after typing.
        for down in [true, false] {
            CGEvent(keyboardEventSource: nil, virtualKey: 48, keyDown: down)?.postToPid(pid)
        }
        RunLoop.current.run(until: Date().addingTimeInterval(0.15))
        if let raw = attr(root, kAXFocusedUIElementAttribute as CFString), CFGetTypeID(raw) == AXUIElementGetTypeID() {
            let target = raw as! AXUIElement
            let targetRole = attr(target, kAXRoleAttribute as CFString) as? String ?? ""
            let subrole = attr(target, kAXSubroleAttribute as CFString) as? String ?? ""
            let id = attr(target, kAXIdentifierAttribute as CFString) as? String ?? ""
            print("After Tab role=\(targetRole) subrole=\(subrole) id=\(id)")
        }

        // A secure text field can deliberately withhold its value. The server
        // accepts the sign-in only after the actual password arrives.
        for code: CGKeyCode in [35, 0, 8, 40, 0, 5, 14, 27, 2, 14, 46, 31] { // package-demo
            for down in [true, false] {
                CGEvent(keyboardEventSource: nil, virtualKey: code, keyDown: down)?.postToPid(pid)
            }
        }
        return
    }
    let field = try control(name, kAXTextFieldRole as String)
    guard AXUIElementSetAttributeValue(field, kAXFocusedAttribute as CFString, kCFBooleanTrue) == .success else {
        throw fail("The \(name) field refused focus.")
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
