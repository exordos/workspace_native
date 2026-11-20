# Testing native changes

Configure from the repository root using the supported Qt kit in the
[development guide](../NATIVE_DESKTOP_DEVELOPMENT.md). To run one isolated
suite for an API change:

```sh
cmake -S packages/native -B build/native -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/native --target api-client-test
ctest --test-dir build/native --no-tests=error --output-on-failure -R '^native-api-client$'
```

Use Qt Test's `-functions` and `-datatags` on the built binary to discover
individual methods and data rows. Record the failing method, inputs and observed
result before fixing a defect. Include the narrow regression and the original
user entry point when a unit test alone cannot establish the behavior.

| Evidence | What to exercise |
| --- | --- |
| Domain or policy | Value types and decisions with deterministic inputs |
| Adapter or service | Real serialization, scoped storage, failures, cancellation and stale callbacks |
| Window | User input, focus, text and the actual rendered desktop surface |
| Package or platform | Launch the installed binary on its target OS and exercise the OS facility |
| Performance | Record runner, renderer, fixture, calibrated budget and temporal traces |

Run the full gate with the documented desktop services before accepting a
release. Use fresh per-test settings, caches, fixtures and ports. Wait for a
specific completion with a deadline. When splitting a suite, compare the sets
of method names and tagged rows, preserve their assertions and update every
CTest selector, CI caller and documented command together. Record skipped tests,
their dependencies and the platform on which they can actually run.

## Migrating the desktop suites

[The controller split](https://github.com/exordos/workspace_native/issues/94)
owns the first migration. Follow the selectors in the checked-out revision of
the [development guide](../NATIVE_DESKTOP_DEVELOPMENT.md). Before separating
another suite, record the current CTest commands and labels:

```sh
ctest --test-dir build/native --show-only=json-v1 > ctest-before.json
build/native/native-window-test -datatags > window-before.txt
build/native/native-e2ee-read-test -datatags > crypto-before.txt
build/native/native-e2ee-ui-test -datatags > crypto-window-before.txt
```

Run the window tools inside the same desktop environment used for the full
gate. Keep the source revision, Qt version, renderer and fixture revision
with the inventory. Qt lists the data tags for the whole executable. Also
record the method arguments in each CTest command, since several registrations
run selected methods from the same executable.

| Current entry point | Next extraction | Coverage to retain |
| --- | --- | --- |
| `native-window-test` | Move a single component's tests and its local fixture to a focused Qt Quick test. Keep sign-in, navigation, cross-surface focus and packaged journeys in the shell tests. | Compare selected method and tag pairs. Run both the focused tests and the original shell scenarios with the release renderer. |
| `native-e2ee-read-test` | Extract reusable peer process, TLS fixture and synthetic identity setup into test support. Give protocol and controller tests a distinct executable entry point. | Run against the existing Node peer and OpenSSL fixture. Preserve protocol expectations, event ordering and failure cases. |
| `native-e2ee-ui-test` | Separate the Qt Quick entry point from the shared translation unit when its fixture is available. Give the UI tests their own explicit sources and desktop dependencies. | Retain UI method selectors, visible read states and accessible interaction. Exercise the real peer and the complete desktop gate. |

For each extraction, compare the before/after selected method and data-tag
sets and explain each added regression. Update CTest commands, labels, timeouts,
CI and the development guide in the same PR. Record full-gate durations and
skips on comparable hosts. Remove the old registration only when its owning
scenarios all run from their new entry points.
