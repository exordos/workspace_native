# Last conversation startup fallback

Issue #148 already has owner-scoped, debounced last-conversation persistence and
canonical topic restoration. Reuse those mechanisms and their account isolation.
The remaining startup behavior is a quiet inbox when a saved stream disappears
or a saved topic or its destination stream returns 403/404. Clear only that stale
owner-scoped route after authoritative failure. Malformed preferences already
fall back to the inbox. Explicit user navigation must retain its normal errors.

Bind the automatic-restore marker to the existing topic-opening intent, with its
owner, generation, catalog, selection and cancellation checks. Other failures,
including unavailable service and invalid server metadata, remain visible and
keep the saved route available for a future launch. No new storage or API is
needed. Verify stream/topic fallback, manual navigation and transient failure
controls, same-account restoration, owner isolation and the actual Main window.

Focused Linux validation: five unavailable-route cases reproduced visible errors
before the fix. A delayed automatic fallback also reproduced reopening an Inbox
that the user had already left; the existing restore ticket now retires that
intent. Eighteen controller checks, all 24 preference checks on XCB, 14 locale
checks and seven actual Main checks passed, including both languages/palettes
and reopening a saved chat in a new Main. The production target builds. The
preferences suite requires a real display for theme/window geometry assertions;
the offscreen attempt failed those two unrelated display checks.

Full controller/window suites, live service, actual secure-store executable
restart, installed packages, Windows and macOS were not run for this change.
