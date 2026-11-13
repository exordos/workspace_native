# Topic viewport layout: candidate for issue #141

## Confirmed measurement defect

`MessageContentRow.settleGeometry()` is called during delegate completion and by
`messageList.forceMeasuredLayout()` before restoring the viewport. Previously it
forced the main body and outer bubble Column, but left the quote's Text objects
and nested Column pending their own polish passes.

After a width change, this allowed the helper to return a quote height of 991 px
while its visible children required 271 px. Viewport restoration could therefore
consume a stale row height. The regression uses the real `Main.qml`, a loopback
server and synthetic quote content; it compares the synchronous frame height
with the measured visible children at three successive widths, without an event
pump between the width change, measurement and assertion.

The candidate measures the quote's author, selected text and rich text, then its
Column, before measuring the outer bubble Column. It retains the existing
virtualization, viewport anchor policy and two-pass restoration. No cache limit,
history API, account ownership, release version or text presentation is changed.

## Relationship to the live freeze

The published 0.18.0 trace in issue #141 has 232 repeated extent cycles with a
fixed 50-row model. The complete local numeric capture additionally shows:

| Cycle | Anonymous child | Width | Initial height | Corrected height | Change |
| --- | ---: | ---: | ---: | ---: | ---: |
| 1 | 71 | 943 | 1557 | 237 | -1320 |
| 1 | 74 | 943 | 361 | 161 | -200 |
| 2 | 76 | 943 | 1557 | 237 | -1320 |
| 2 | 79 | 943 | 361 | 161 | -200 |
| 3 | 81 | 943 | 1557 | 237 | -1320 |
| 3 | 84 | 943 | 361 | 161 | -200 |

These are different item incarnations, not repeated width changes on one item.
The two corrections total the observed -1520 coordinate drift per cycle.
They need not total the 9401 difference between the two content-height estimates:
[ListView estimates unallocated rows from allocated delegates](https://doc.qt.io/qt-6/qml-qtquick-listview.html#variable-delegate-size-and-section-labels).
The recorded child objects do not identify the inner binding that changed, so
this correlation does not prove that the quote measurement defect initiates the
entire live freeze. Live-service acceptance remains open.

## Probe and acceptance

The independent opening harness in draft PR #151 remains useful for the full
freeze investigation. Its reply fixture currently writes top-level `content`,
which `parseMessage` ignores in favor of `payload.content`. A projected-quote
assertion is needed there before its synthetic results establish quote coverage.
This candidate does not depend on merging that harness.

Focused checks:

```sh
native-window-test measuredQuoteHeightIncludesNestedTextBeforeViewportRestore
native-window-test replyBannerAndQuoteJumpUseTheComposer \
  editQuotedMessageShowsTheQuoteChipOverTheComposer
```

The measurement test fails on the unchanged production QML and passes with this
candidate. This narrow regression is not a RED reproduction of the full freeze.
Real-server verification must still establish continued GUI heartbeats and frame sync,
bounded delegate/RSS growth, a stable tail or anchor, and usable rapid scrolling
and repeated topic switching on the affected scene before #141 is closed.
