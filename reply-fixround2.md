Confronted the residual claim with a direct timer-interval pin plus a RED witness on this head (0e5a8049).

The witness now asserts the contract itself, not a floor sample: `controlsHideTimer` carries a test-only `objectName` seam and the case does `QCOMPARE(hideTimer->interval(), 2500)`, so any shortened countdown fails on the pin instead of slipping under a 1900 ms floor sample.

Measured locally on the head:

- GREEN: `native-window-test mediaViewerVideoControlsAutoHideReturnsOnMoveAndPause` -> `Totals: 3 passed, 0 failed` (rc 0).
- RED-as-expected witness: patched `NativeMediaViewer.qml` `interval: 2500` -> `interval: 2000`, rebuilt the same binary, re-ran the same case -> `Totals: 2 passed, 1 failed` (rc 1), `FAIL! : NativeWindowTest::mediaViewerVideoControlsAutoHideReturnsOnMoveAndPause() Compared values are not the same` - i.e. the interval pin catches a 2000 ms countdown deterministically. The hack was reverted and the tree rebuilt clean after the witness.

So a `countdown < 2500 ms` is no longer accepted: the interval is pinned to the reference value and proven RED when shortened.
