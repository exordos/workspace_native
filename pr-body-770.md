Closes #770.

## Root cause
Scene destructors pump the event loop (the frame/focus acknowledgement inside
`closeIncubatedTestWindow`) after the case's later-declared locals are
destroyed. A late in-flight client request landing in that pump re-enters the
case's `[&]`-captured handler chain and calls the destroyed `base`
`std::function` copy. CI shows `std::bad_function_call` (abort); the local
gdb capture shows the same call jumping through reused freed stack memory
(condensed bt + evidence in #770).

## Reproduction (pre-fix, on tip 93c7dbb6)
The two neighbouring dialog cases back-to-back under
`gdb -batch -catch syscall exit_group` aborted the binary within 3 iterations
(3 distinct local crash shapes, same seam). See issue #770 for the captured
backtrace and the earlier CI runs (part8/part15 signatures, docs-only PR
victims #767/#768 + one 0.48.1 packaging retry).

## Fix
Tests-only:
1. Every scene destructor that pumps the teardown loop (ChannelInfoWindowScene,
   ProfileWindowScene, NativeWindowScene, PollBubbleScene,
   WindowStreamColorScene, WindowChatHeaderCountersScene) quiesces its mock
   server (`server.handler = {}`) before `closeIncubatedTestWindow`, so the
   pump can only hit the safe empty fallback mock-server.h already provides.
2. New deterministic regression case
   `lateTeardownRequestDoesNotReenterDeadCaseLocals`: a raw QTcpSocket client
   parented to the scene's controller delivers its request via a 0 ms timer
   that fires only inside the destructor pump - provably after the captured
   locals died - with an arrival counter that asserts the seam is exercised
   every run (no vacuous green).

## Verification
- RED (fix stripped): the seam case aborts the process at the pump
  (rate recorded in repro5-driver.log RED_SUMMARY).
- GREEN (fix restored): seam case green 8/8; seam loops
  (InvalidEndpoint x16, SendsOnlyPrompt x16, profile exit-teardown x12)
  fully green - exact counts recorded in this PR before merge review.
