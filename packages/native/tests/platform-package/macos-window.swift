import Foundation
import CoreGraphics

let app = Process()
app.executableURL = URL(fileURLWithPath: CommandLine.arguments[1])
try app.run()
defer {
    if app.isRunning {
        app.terminate()
        app.waitUntilExit()
    }
}
let deadline = Date().addingTimeInterval(90)
var visible = false
while Date() < deadline && app.isRunning && !visible {
    let entries = CGWindowListCopyWindowInfo([.optionOnScreenOnly, .excludeDesktopElements], kCGNullWindowID) as? [[String: Any]] ?? []
    for entry in entries {
        guard let pid = entry[kCGWindowOwnerPID as String] as? Int,
              pid == Int(app.processIdentifier),
              entry[kCGWindowLayer as String] as? Int == 0,
              let bounds = entry[kCGWindowBounds as String] as? [String: Double],
              let width = bounds["Width"], let height = bounds["Height"],
              width >= 400, height >= 300 else { continue }
        print("Observed packaged macOS process \(pid) with visible window \(Int(width)) x \(Int(height)).")
        visible = true
        break
    }
    if !visible { Thread.sleep(forTimeInterval: 0.25) }
}
if !visible {
    throw NSError(domain: "WorkspacePackageSmoke", code: 1, userInfo: [NSLocalizedDescriptionKey: "The package did not present its window."])
}
