# Native architecture and ownership

Start with the row that owns the state you want to change. Paths below refer to
`packages/native`. The application composes Qt objects on the GUI thread in
`src/main.cpp`. The executable links the UI library, which links the core
library. The core also has infrastructure dependencies, including Qt Network,
Qt SQL and OpenSSL. Pure rules should have the smallest target their inputs need.

| Change | Owner and entry point | Focused test or contract |
| --- | --- | --- |
| Authentication, requests and session replacement | `src/session.cpp`, `src/api-client.cpp`, `src/session-vault.cpp` | `session-test`, `api-client-test` |
| Chat selection, paging and events | `src/native-controller.cpp`, `src/realtime-client.cpp`, `qml/Main.qml` | [History and anchors](../NATIVE_DESKTOP_HISTORY_PREFETCH.md) |
| Cache ownership and draft recovery | `src/native-cache-service.cpp`, `src/native-draft-service.cpp`, their storage adapters | `native-cache-service-test`, `native-draft-service-test` |
| Protected messages and key storage | `src/native-chat-crypto.cpp`, `src/native-chat-key-vault.cpp` | [Crypto contract and verification](../NATIVE_DESKTOP_CRYPTO.md) |
| Calls and native overlays | `src/native-call-bridge.cpp`, `src/call/`, `qml/Main.qml` | `native-call-test`, `native-window-test` |
| Desktop notifications and attention | `src/native-notifications.cpp`, `src/native-desktop.cpp` | `native-desktop-test` and installed desktop checks |
| Diagnostics providers and presentation | `src/native-logging.cpp`, `src/native-diagnostics.cpp`, `qml/NativeDiagnosticsPage.qml` | [Current providers](../NATIVE_DESKTOP_DIAGNOSTICS.md), [diagnostics page](../NATIVE_DESKTOP_DIAGNOSTICS_PAGE.md) |

Keep decoded server values and authorization in the owning service. Give QML
components explicit properties and signals. Keep local editing and focus state
inside the component. Supply a variable dependency such as a store or transport
where the feature is wired. Give each asynchronous operation an owner, a scope
(account, server and project when applicable), a cancellation path and a bounded
lifetime. Recheck that scope when an external callback completes. Document the
owner of retries, ordering and state transitions.

For an extraction, move one behavior with its tests, preserve stored and wire
formats, and record its immediate callers. Add a small CMake target when an
independent feature has a narrower dependency set. Exercise the original public
entry after moving the implementation. Record a temporary compatibility adapter
in the feature contract with its callers and removal condition.

## Reviewing a change

- Can the reviewer locate the owner, its direct dependencies and public entry?
- Are persisted or asynchronous results confined to their original scope?
- Does the test exercise the observable failure at the appropriate level?
- Does the change preserve selectors, ownership and cancellation during migration?
- Do the diagnostic fields and the updated contract match the shipped behavior?

Describe any exception in a reviewed decision record with its owner, reason,
affected paths and a condition for revisiting it. See the [decision template](../decisions/template.md).
