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
