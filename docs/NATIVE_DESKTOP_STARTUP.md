# Startup directory projection (issue #157)

The startup controller publishes cached and authoritative project directories.
New-chat and channel-info popups exist in the main QML scene even while closed.
Their user pickers must only project the directory while the corresponding
picker is visible: the direct-chat tab, the channel-creation tab, or the separate
Add members popup. Inactive tabs keep empty models. Opening a picker reads the
current directory and search/selection state through its existing binding.

This avoids filtering, sorting, converting and publishing whole directory lists
for hidden controls on generic controller state notifications. It does not defer
verified directory ownership, change authentication, discard cache entries, or
introduce a second directory model. The controller remains responsible for the
same authority checks; hidden UI data is derived again on demand.

## Focused regression and measurement

Build `native-window-test` and run it on a visible Linux desktop:

```sh
cmake --build packages/native/build --target native-window-test --parallel 2
QT_QPA_PLATFORM=xcb packages/native/build/native-window-test \
  startupDirectoryDoesNotPopulateHiddenPickers
```

The cold and warm-cache cases use 3,000 synthetic directory users and 50
variable-height messages. The warm case seeds an owner-scoped disk cache and
reopens it through the normal asynchronous cache service. Both hold the directory
HTTP response pending while displaying a conversation, dispatching real key
input, and checking event-loop liveness. Directory publication must preserve
composer text, focus and the selected topic. Hidden picker models must stay empty;
opening, tab switching, filtering, closing and reopening must still populate the
appropriate visible picker. Existing channel-info interaction cases cover the
Add members popup's selection/reset and request behavior.

The test logs window-exposure time, time to visible conversation content, the
largest 1 ms heartbeat gap during initial hydration, and directory-publication
elapsed time and heartbeat gap. These are measurements, not universal timing
gates: loaded shared runners, QML disk caches and the rendering backend influence
them. The deterministic regression gate is absence of hidden directory models,
combined with working visible interaction. Preserve per-run logs outside this
repository and compare repeated runs on the same host/build configuration.

## Message projection during pending startup

An empty message-action target no longer reads the full message projection. The
cache-status text uses the message list's existing current-route snapshot for
its empty/nonempty distinction. Active menu and reaction lookups still use the
controller projection, including its ownership and mutation checks. A target
may remain selected after a menu closes because a confirmation dialog can still
need it; this change does not eliminate every later projection read.

On one Linux/XCB host using the same Debug build, eight interleaved
baseline/candidate process pairs ran
the 3,000-user, 50-message fixture above with its directory response pending.
A temporary getter counter reported 84 to 0 projection reads during 39 synthetic
key events on the cold case and 94 to 2 on the seeded-cache case. Median time
per event excluding the first key changed from 6.98 to 3.96 ms cold and 7.47 to
4.14 ms seeded; every pair improved on that metric. The first key, which can
include pending layout work, had no reliable improvement, nor did the window or
content metrics. The event timing includes test dispatch and synchronous GUI
work and is not a production typing-latency or process-start measurement.

The `emptyMessageActionTargetDoesNotBuildMessageProjection` regression executes
the actual packaged QML function with a counting getter. The
`savedStatusTracksMessagesWhenSwitchingChats` window test checks the banner
against nonempty and empty saved histories, a delayed obsolete chat response,
and the end of revalidation. The existing cold/warm startup test exercises real
key input and focus while the directory response remains pending.

## Remaining startup acceptance

This is a focused synthetic startup improvement, not complete acceptance of
issue #157. It does not measure process launch before the test window, real secure
credential restoration, authenticated production traffic, image loading, full
large-channel bootstrap, Wayland frame pacing, prolonged CPU/RSS settling, or
packaged/cross-platform startup. Opening a large picker still performs its real
filter/sort work; that separate visible interaction must be profiled before any
further optimization. The recurring real-service acceptance in issue #158 remains
necessary for the complete launch-to-usable experience.
