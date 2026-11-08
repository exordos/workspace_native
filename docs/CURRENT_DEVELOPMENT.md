# Current native development

The imported baseline remains recorded in `docs/provenance/import-v34.json`.
This branch adds ordinary native channel responses to a current participant's
foreign key request. Current participants already holding the exact requested
generation can Grant or Reject. Owners and administrators retain Reject without
the requested local key. Grant reads the exact generation already held in the
native vault; the current stream key can be null. Typed payloads, canonical
placement, the exact default-topic target, request/device binding and retained
controller Check/Retry reuse the existing protocol. The existing Main buttons
now recover retryable saved responses. Ordinary key acquisition, bootstrap,
provider issuance and the remaining issuer-recovery cases are separate parity
work.

Ordinary plaintext windows skip shared issuer-authority preparation when they
contain no service controls. An unfinished owned response remains an exception,
so explicit Retry can recover after a trust reset. Actual no-work and real WSS
regressions passed after reproducing both the extra reads and the problem with
a naive empty-window guard. The complete Core261 run passed on that earlier
authority snapshot; [authority checks](provenance/ordinary-authority-checks.json)
preserve its scope and an unexplained historical cache-fixture startup failure.

Missing foreign users have the pinned desktop's `#` plus first eight UUID
characters as their generic message author label. Known names, username fallbacks
and existing own-message behavior are preserved. Controller and actual visible
Main regressions reproduced the difference and passed after the one-line change.

A full message move now retains an already-known owned History body even when no
saved destination window accepts it. The existing indexed traversal supplies one
candidate; the existing bounded Body storage saves its canonical projection.
Partial masks, viewer ownership, deletion, monotonic reads and unmasked metadata
are preserved. Existing Body ages and History ages/cursors remain unchanged;
a newly materialized Body records the current canonical event. This addresses one
cache behavior from upstream `61dacef`, without claiming all of its routing changes.

The moved-body local checks passed Cache99, Cache Service53 and Controller748, without
warnings or skips. The standalone application also built. The original Cache and
Controller move regressions failed before the fix and passed afterward, including
disk reopen and no invented destination page. Scripted-clock, independent viewer,
read/mask and entry-budget controls passed; see
[moved-body checks](provenance/moved-body-checks.json). Later Core checks include
the manager-response change below; sanitizer and live-service acceptance remain
open.

Ordinary manager Grant reproduced the missing action in both the controller and
the actual Main window before the change. Focused checks now pass for owners with
a member binding and administrators, exact-generation and current-member fences,
revocation, and GET-only status after local key deletion. An independent pinned
Source recipient imports the native Grant and decrypts a public Native ciphertext
probe without receiving the private key. Actual English/light and Russian/dark
Main mouse clicks produce one canonical response, remove the actions and do not
duplicate the POST on a second click. Existing ordinary Reject and encrypted
issuer controls pass. The full Core269 run passed in 601.371 seconds. A matched
pre-click Source comparison uses actual public Native inputs and the pinned
production Source row/actions; all four button rectangles differ horizontally by
less than 0.3px, with identical y/height. This accepts that row's presentation,
not Source authority/crypto/dispatch or the complete shell.

Ordinary holder responses reproduced missing Grant/Reject controller capability
and the missing actual Main Grant button before the change. Current-self and
requester membership, exact A rather than B, fresh authority and encrypted/global
manager limits remain enforced. Real Main Grant and Reject clicks pass for the
new member holder and existing owner/administrator rows. A pending holder can act;
a fresh terminal row denies all four actions. The earlier full run's two obsolete
pending-holder prohibitions remain recorded.

Two additional regressions reproduced retained Reject staying busy after local A
loss: during synchronous sending publication before any POST, and while explicit
Retry awaits its acknowledged current-request GET after an unapplied failed POST.
Both now settle the same current intent through the existing post guard. GET-only
Check and explicit same-wire Retry retain their existing ownership rules; Reject
does not acquire Grant's permanent material-loss pin. These recovery checks are
**controller-only at that holder snapshot**. The later Main change below reuses
the existing labels for retryable matching responses. No separate Check/Retry
controls were added.

The final holder snapshot passes complete Core274 in 613.800 seconds
and the eight actual Main initial-action cases in 13.206 seconds,
without QtTest warnings, skips or runtime stderr in those groups. Its complete UI
run is **not accepted**: after 93 completed passes it crashed with SIGSEGV in
`uiServiceHistoryCommitAndRealtimeHaveDifferentExpansion`, before the new member
Main cases. One original-order prefix under a debugger passed 96 cases without a
signal; the cause remains unknown and no crash fix is claimed. The earlier
GREEN4 complete UI131 pass predates the final one-line admission routing guard
and cannot replace this failed final run. The Source/member comparison uses the
same actual public inputs;
the two button rectangles have equal y/height and less than 0.3px x difference.
The compared text/actions do not establish full shell or Source runtime
role/crypto/dispatch parity. [Holder checks](provenance/ordinary-holder-checks.json)
retain the actual REDs, intermediate failed scope run, accepted groups and exact
presentation boundary. Historical warning causes below remain open.

A later Main change lets a new explicit click of the same eligible Send key or
Reject label recover the matching saved response. A status GET runs first; an
already applied response settles without another POST. Only a fresh absent
response proceeds through the existing fresh-authority/current-request checks
and exact-wire retry. Four actual mouse scenarios cover G/J with applied or
unapplied 503s and independent Source recipient completion. A second real Source
request and the opposite action cannot borrow that permission; contained mouse
inputs leave traffic and the saved identity unchanged. These groups passed 6 and
3 Qt cases; the existing initial manager/holder/encrypted controls passed 16.
Counts include setup/cleanup per group. [Main recovery checks](provenance/same-action-main-recovery-checks.json)
record the actual four-row RED and focused results. Permanent Grant material-loss
and status-only flows, switching R/action, Source failure-toast presentation and
complete UI acceptance remain open. The existing full-run crash and separate
history-expansion failure are not claimed fixed by this QML change.

A controlled real-window merge reproduced an unsafe test-side delegate lookup:
after deleting the separator, Core retained two service messages while the view
merged them into one header and destroyed the other. The old property predicate
crashed; its captured stack points to that null lookup. The test helper now
requires a present item for either expected state and reacquires the UUID after
realtime updates. The actual merge/absence/remount regression passes. All seven
original expansion oracles and sixteen QTRY calls remain. A separate focused
run still failed one original live-expansion expectation (10 passes, one failure),
now as a normal assertion; its model/readiness cause is under investigation.
This does not establish the cause or resolution of the earlier full-run crash.
[Predicate checks](provenance/service-predicate-checks.json) retain both results.

The first full UI run passed 124 cases and failed four obsolete full-UUID author
lookups in the locked-message geometry test. Its lookup now requires the accepted
Source `#` plus eight UUID characters and checks that exact controller caption;
every geometry assertion is unchanged. The focused six-case check passed. The
next original-order UI run passed 127 cases and failed one: Qt reported two items
still being created at engine destruction in the automatic offscreen-collapse
test. A passive diagnostic reproduced that warning and observed one public
asynchronous creation still pending while the window hid. It does not identify
both private Qt creations. A controlled real-window regression then failed with
one pending item at first Hide. Its test-only close wrapper now waits for an
ordinary visible frame and completion of public incubation before the existing
focus/close cleanup. Product code, asynchronous creation and warning checks are
unchanged. The controlled witness and original offscreen case passed together.

The subsequent complete original-order UI run passed 128 cases and failed one
different slot: two Wayland text-input warnings were followed by an observed
empty model where the large-history fixture expected 500 messages, before that
slot creates its window. The cause is unproven; this run is **not accepted**.
The new close wrapper is first invoked later in the suite. No warning is
suppressed or rerun for acceptance. [Teardown checks](provenance/ui-teardown-checks.json)
preserve the actual warning reproduction, controlled regression and this failed
full run. The focused Main and comparison checks above passed independently.
[Grant checks](provenance/ordinary-grant-checks.json) preserve the earlier failures
and exact accepted scope.

The earlier Window run passed 224 functional cases, but its quality gate remains
**not accepted**: one test sent a mouse event outside a 700px window. Isolated
archive and original-order prefix diagnostics passed without reproducing it.
Separately, one ordinary owner Main capture reported `QMutex: destroying locked
mutex`; a diagnostic run passed without an emitting stack. Neither warning is
suppressed or claimed fixed. [Author checks](provenance/message-author-checks.json)
retain the failed observations. The previous complete UI126 run predates the
authority, author-label and moved-body fixes.

Screenshots compare the service row and the author-label change. Full shell,
presence/counts, Source runtime role/action acceptance, live service, packaging,
complete Linux and other platforms remain open. Linux comes first, with a
configurable 10 GB disk-cache default. Full acceptance requires the complete
relevant groups and warning checks.
