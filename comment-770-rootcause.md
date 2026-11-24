## Root cause identified (reproduced locally with backtrace, 2026-11-24)

A targeted seam repro (the two neighbouring dialog cases run back-to-back under
`gdb -batch -catch syscall exit_group`) crashed the binary within 3 iterations
on the tip gate tree (93c7dbb6). Captured backtrace (condensed, full log kept):

```
MockServer readyRead lambda            mock-server.h:86   (handler ? handler(request) : Response{})
  test-case handler lambda             native-window-test.cpp:25464   -> return base(request);
  std::function<Response(const Request&)>::operator()   <-- DEAD local `base` (stack)
  wild call / bad_function_call        <-- abort: the CI signature
...
QTest::qWaitFor (event pump)           closeObservedTestWindow
closeIncubatedTestWindow
~ChannelInfoWindowScene                native-window-test.cpp:2337   <-- teardown pumps the loop
case function epilogue                 <-- locals declared AFTER the scene are already destroyed
```

Mechanism: the scene is declared first in a case, and the case's handler chain
(`const auto base = scene.fixture.server.handler; ... saveBodies` captured by
`[&]`) consists of locals declared after it. At scope exit those locals die
FIRST, then `~ChannelInfoWindowScene` runs
`closeIncubatedTestWindow`, which pumps the event loop (frame/focus
acknowledgement). A late in-flight client request landing in that pump
re-enters the case's handler chain and invokes the destroyed `base`
`std::function` copy: in CI it throws `std::bad_function_call` (abort), locally
the freed stack memory was already reused and the call jumped to a wild target.
Load only matters because it delays the final request past the case body.

Fix shape (lane `cassi/teardown-race-770`): every scene destructor that pumps
the teardown loop (ChannelInfoWindowScene, ProfileWindowScene,
NativeWindowScene, PollBubbleScene, WindowStreamColorScene,
WindowChatHeaderCountersScene) now sets `server.handler = {}` as its first
statement, so the teardown pump only ever sees the safe empty fallback.
A deterministic regression case (`lateTeardownRequestDoesNotReenterDeadCaseLocals`)
drives exactly this seam: a raw client parented to the scene's controller
delivers a request via a 0 ms timer that can only fire inside the destructor
pump, i.e. provably after the captured locals died. RED/GREEN verification +
seam loops are running; PR follows with the measured results.
