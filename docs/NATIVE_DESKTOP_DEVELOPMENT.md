# Native desktop development

The native preview lives in `packages/native`. It uses C++17, Qt 6.10 or later,
Qt Quick, and CMake 3.24 or later. It does not run the React application or load
Workspace in a WebView. The current scope and remaining acceptance work are in
[Native desktop parity](NATIVE_DESKTOP_PARITY.md).

## Build and run

Install a matching Qt development kit for the target operating system, including
Core, Gui, Widgets, Network, Qml, Quick, QuickControls2, QuickDialogs2, Svg, WebSockets, Sql and
Test, plus the matching image-formats plugins (including WebP), Qt6Keychain, MD4C (including its HTML renderer), OpenSSL 3.2 or later
(Crypto and SSL), and Qt's `lrelease`
translation compiler. Test builds also use Qt's `lupdate` to check current public
source strings against the reviewed translation catalog. The runtime needs the QML modules used by Quick
Controls, Layouts, Dialogs, Effects and the dialogs' folder-list model. Real remembered
sessions require a functioning operating-system secure credential service.

On a Debian/Ubuntu development machine:

```sh
sudo apt-get install --no-install-recommends cmake ninja-build patchelf qt6-base-dev qt6-image-formats-plugins \
  qt6-declarative-dev qt6-svg-dev qt6-websockets-dev qt6-l10n-tools qt6-translations-l10n qt6-wayland qtkeychain-qt6-dev libqt6sql6-sqlite libmd4c-dev libssl-dev \
  qml6-module-qtquick qml6-module-qtquick-dialogs \
  qml6-module-qt-labs-folderlistmodel \
  qml6-module-qtquick-controls qml6-module-qtquick-layouts qml6-module-qtquick-effects \
  qml6-module-qtquick-window qml6-module-qtquick-templates \
  qml6-module-qtqml-workerscript
```

Use a distribution or Qt kit that supplies Qt 6.10 or later; installing older
distribution packages does not meet the CMake version requirement.

From the repository root:

```sh
cmake -S packages/native -B packages/native/build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build packages/native/build --parallel 4
packages/native/build/workspace-native
```

On Windows or macOS, select the installed Qt kit with `CMAKE_PREFIX_PATH` when
CMake cannot locate it. Use a compiler matching that kit. Native builds, signing,
installation, and packaging acceptance on those operating systems remain open;
a Linux build does not establish them.

The current Linux development build has exercised synthetic transport/domain,
credential-backend and visible Quick-window checks. Packaged installation,
live-service behavior, full visual acceptance and actual secure-store acceptance
remain open on Linux, Windows and macOS. See the
[platform matrix](NATIVE_DESKTOP_PARITY.md#acceptance-plan) for acceptance criteria.

## Stage and inspect the runtime

The application embeds its QML as an executable module. Qt's
[QML deployment command](https://doc.qt.io/qt-6.10/qt-generate-deploy-qml-app-script.html)
scans the actual imports and installs their runtime plugins. Linux installation
uses `patchelf` for copied plugins and
[the copied Qt/keychain/image-codec libraries](../packages/native/cmake/FinalizeLinuxRuntime.cmake.in).
Some distribution Qt builds remove library RPATHs; executable RUNPATH alone does
not resolve their transitive dependencies inside the package.

Always install into a disposable staging directory rather than the system:

```sh
cmake --install packages/native/build --prefix /absolute/path/native-stage
python3 packages/native/tests/verify-installed-runtime.py /absolute/path/native-stage
# With BUILD_TESTING enabled, also exercise the packaged TLS/crypto providers:
python3 packages/native/tests/verify-installed-runtime.py /absolute/path/native-stage \
  --probe packages/native/build/native-runtime-probe
```

The [installed-runtime check](../packages/native/tests/verify-installed-runtime.py)
verifies required QML modules, actual plugin files, platform and SQLite driver
plugins, `qt.conf`, and the bundled Markdown parser and license. On Linux both
`libmd4c` and `libmd4c-html` must resolve inside the staging directory, including
the HTML renderer's transitive dependency on the parser.
The matching OpenSSL Crypto/SSL libraries and license are copied from the selected
SDK. The executable directly initializes SSL so the loader resolves that private
pair before a host dependency can bring in a system SSL library. The runtime
check guards this direct dependency. Its optional noninstalled probe exercises
message/grant encryption, PNG/JPEG/GIF/WebP decoding and the Qt TLS backend, checks their actual mapped
library paths, and removes its temporary staged executable afterward. It uses
synthetic values and performs no account or network requests.
Linux deployment explicitly includes the Wayland platform, XDG shell and client
graphics/decoration plugins in addition to XCB. Plugin selection uses Qt 6.10's
deployment interface. A successful developer-window test does not prove those
dynamically loaded plugins are present in the package.
On Linux, launch the staged executable visibly and pass its PID with `--pid PID`
to check that loaded Qt/keychain/QML/plugin/Markdown libraries come from that staging
directory. Give the launch separate `XDG_CONFIG_HOME`, `XDG_DATA_HOME` and
`XDG_CACHE_HOME` directories so acceptance cannot consume or modify a real saved
session. Close that process and remove temporary settings when finished.

Staging checks reproduced missing QML imports, SQLite/Markdown dependencies and
transitive Qt libraries loading from the host. A later actual Wayland launch also
failed because Qt's default deployment had copied only XCB. These cases are now
covered by the installed-runtime check and explicit deployment selection.

The frozen V13 Linux stage with the tray passed the nine-module/plugin check and
showed its anonymous sign-in window on the current Wayland desktop. All 42
observed Qt/keychain/QML/plugin/Markdown libraries mapped inside that stage.
Alt+F4 hid it while its process stayed alive. A second launch exited with code 0
and restored the same primary process; Ctrl+Q then ended that process with code 0.
Its isolated settings/cache/data directories were removed. Earlier, a
temporary staged cache probe also completed a SQLite write/read with its
Core/Sql/SQLite driver inside the package, followed by helper/database cleanup.

These checks establish startup and this anonymous-window lifecycle on this host.
They do not establish full functional behavior, a clean-machine installation,
relocation across machines or distributable installer/signing/update acceptance.
The unregistered staging app still reports a host-portal app-ID warning; actual
portal dialogs and an installed desktop registration remain unaccepted.

The later V14b integrated run passed all 19 CTest groups, including 218 controller
cases and 74 window cases. Its stage passed the ten-module runtime check, the
synthetic message/grant and TLS probe, and an actual anonymous Wayland launch.
All 46 observed Qt/keychain/QML/plugin/Markdown/Crypto/SSL mappings came from the
stage. Close/hide, same-process second-launch restoration and Ctrl+Q exit passed
again; the process and isolated settings were removed. The probe regression first
reproduced a host `libldap` dependency loading system SSL while Crypto came from
the stage, and passed after the executable retained its direct SSL dependency.
This remains a check on the current Linux host, not installer or full-account
acceptance.

The V15 integration adds the ordinary stream aggregate and initial encrypted
key-vault foundation. All 20 regular CTest groups passed in 272.21 seconds, with
675 cases including suite initialization/cleanup: 252 controller, 84 visible
window and 21 key-vault cases among them. The ten-module staged check, synthetic
message/grant/TLS probe and anonymous Wayland lifecycle also passed; all 46
observed runtime mappings resolved inside the stage. Closing hid the window,
a second launch restored the same process, and Ctrl+Q exited with code 0.
The process and isolated settings were removed. The separate opt-in key-vault
test passed with a real, private GNOME Keyring provider as described in
[Native cryptography](NATIVE_DESKTOP_CRYPTO.md#initial-durable-key-vault).
This does not connect the vault to encrypted-chat presentation or establish
live-server, full feature/pixel or installer acceptance.

The later frozen V16b integration passed all 20 groups in 448.37 seconds,
with 945 Qt cases including initialization/cleanup. It included ordinary channel
creation, the 48-case key vault and the then-current 54-case response cache.
The staged runtime and anonymous Wayland close/restore/quit checks passed.

The frozen V17 direct-chat/cache integration is **not accepted**. Its 20-group
run completed in 554.22 seconds: 19 groups passed, including all 591 controller
cases, but the window group reported 135 passes and one failed case. The
unselected-channel sidebar topic form did not become visible after its context
menu action. An instrumented isolated check and a later complete 136-case window
run passed, without reproducing the cause; neither run resolves this failure.
The exact frozen
source and failed logs are retained. Separately, the same core passed 705 cases
both regularly and with ASan/UBSan/LSan, without skips or diagnostics. That count
covers Controller, Domain, Cache57 and Cache Service44, not the QML window suite.

The subsequent topic-publication change passed all 612 controller cases both
regularly (379.976 seconds) and with ASan/UBSan/LSan (434.351 seconds), without
skips or diagnostics. Its focused 72-case run covers publication after navigation,
token/ACL/cache replacement, exact attempt identity, subscriber destruction and
GET-only recovery. A topic form can bind the admitted UUID through
`topicCreationAttemptId` and independently track the current owner/runtime through
`topicCreationContextKey`; the existing stream context key remains the target ACL
witness. These checks cover the controller and local HTTP/WebSocket fixtures.
The global topic form now uses that flow. Matched English/Russian light/dark
component captures preserve the reference's dialog, selector, input and dropdown
rectangles; font/shadow and full-shell differences remain. The later frozen V18
window run is **not accepted**: 167 cases passed and one failed in 190.851 seconds.
The sidebar form again did not open, this time in the 128-codepoint name case;
all five menu input/action signal counts were zero. The two earlier tab/layout test errors were
corrected separately, with a controlled rendered-frame proof for layout readiness.
Their correction does not explain the menu failure or establish a product fix.

A later low-overhead input trace reproduced the V18 failure: the actual press was
at y=226 while the popup occupied y=203..213. Its row was already 36px high, but
the popup still had only its 10px padding because ListView content height was
zero. The menu closed before later route invalidation; no action signal fired.
A controlled rendering probe observed the complete 46px popup before the first
render, so it did not prove that a 10px frame was displayed. The correction is
test readiness: wait for the real source row's full layout and containment before
the existing real mouse click. The deterministic helper regression first failed
at 10px, then passed at 46px. One original-order 23-function prefix passed all
98 cases in 109.485 seconds. Product QML and its menu height remain unchanged.
The original V17 failure lacks this immediate input trace and is retained.

Two separate visible regressions reproduced direct-chat captions retaining UUID
fallbacks after the user directory arrived and after a peer rename. The controller
now notifies the stream model when a displayed peer name changes. Both scenarios
and all 612 controller cases passed; eight real OpenGL component captures retained
the existing form geometry in both locales and themes. These reviewed changes
and the menu test correction are combined in frozen V18c. Its complete 20-group
regular run passed in 586.986 seconds, with 1155 Qt cases including suite
initialization/cleanup, 612 controller cases and 171 window cases. The release
stage passed the ten-module dependency check and synthetic TLS/message/grant
probe. Its actual anonymous Wayland launch mapped all 46 observed runtime
libraries inside that package. Close/hide, same-process second-launch restoration
and Ctrl+Q exit passed; the process and private settings were removed. This
accepts the coherent local component run and this anonymous lifecycle on the
current host. Archive, encrypted content, complete pixel/feature parity,
live-account acceptance and distribution/installer acceptance remain open.

A separate frozen Archive core candidate passed four complete groups in both
regular and ASan/UBSan/LSan builds: Controller 667, Cache 58, CacheService 47 and
Domain 13 cases, including suite initialization/cleanup. All 785 cases passed
in each mode without skips or sanitizer diagnostics. The frozen 237-file input
remained unchanged, and its test processes and temporary runtime directories
were removed. Its retained-catalog correction uses one existing worker put to
save a local projection and revoke freshness, including a real 64-control queue
pressure regression. The earlier four-failure run and subsequent pressure failure
remain preserved. This is private core acceptance only: the shared V18c source
and its 20-group result above do not yet include Archive. Separate UI and
message-placement integration gates remain in progress.

The coherent V18b source includes those test-only corrections and the same product
QML. Its release build, installation, ten-module dependency check and synthetic
TLS/message/grant probe passed. An actual anonymous Wayland launch mapped all
46 observed runtime libraries inside the stage. Close/hide, second-launch
restoration of the same process and Ctrl+Q exit passed; the process and isolated
settings were removed. A five-second idle sample recorded no CPU ticks and no
physical read/write-byte increments; RSS was approximately 224 MiB. This sample
does not establish a memory peak, network/API traffic or authenticated workload
performance. The subsequent 19-group component run passed in 415.696 seconds,
including all 612 controller cases. It explicitly excluded the known failing
window group; no combined 20-group or full-application acceptance is claimed from
it.

The V17 stage passed the bundled TLS/HPKE probe and all 46 observed runtime
library mappings resolved inside the package. Anonymous Wayland close/hide,
same-process second-launch restoration and Ctrl+Q exit passed; its process and
private settings were removed. A process-scoped accessibility probe exposed
52 nodes through AT-SPI. A separate anonymous probe verified the password's
numeric protected-text role and `Password` description; its accessible name was
empty. Reader announcements and complete screen-reader acceptance remain
unverified. These checks do not
accept live-account creation, desktop registration, an installer or full parity.

The packaged Qt translation regression separately reproduced English `Cancel`
under the Russian locale: deployment merges Qt catalogs into `qt_ru.qm`, while
the earlier binding requested only `qtbase_ru.qm`; the generated Linux `qt.conf`
also omitted the actual `share/qt6/translations` path. The locale binding now tries
the merged catalog first, retaining the separate SDK catalog as a fallback, and
Linux finalization records the deployed path. The installed-runtime checker guards
that file and path. A probe using the actual staged runtime now returns Russian
Qt and application labels; it does not establish translated host-portal dialogs.

## Session and preferences

Enter an HTTPS server origin, sign in with username/password and an optional OTP,
then choose an accessible project. The client verifies the issued project/user
through IAM introspection before showing messenger data. Access credentials remain
in the C++ transport/session layer. After verified user/project introspection,
[Session](../packages/native/src/session.cpp) remembers the refresh credential in
[QtKeychain](../packages/native/src/session-vault.cpp), with plaintext fallback
disabled for reads, writes and deletes. A private settings file contains only safe
saved-account metadata; its identity key binds canonical HTTPS origin, project and
user. Passwords and one-time codes are not saved and are cleared from the form.

The sign-in screen lists real saved project/login/server metadata. Continue reads
the secure store, refreshes and verifies the expected identity before entering the
shell. Remove and explicit sign-out forget the selected saved identity. Offline
restoration and internal sign-in resets preserve saved accounts; terminal rejected
credentials invalidate their own saved entry. A secure-store warning is visible
without cancelling a verified session or its realtime snapshot. Actual OS keychain
permission/lock/restart behavior and live restoration still need acceptance.

The profile trigger prefers the current viewer's verified project-directory
first/last name or username, with verified IAM display name/username/email as a
fallback. The email and identity remain the verified IAM whitelist; the directory
name does not change the account UUID or credential identity. New sign-ins and
sign-out clear the previous profile; identity mismatches never publish the
rejected metadata. The source-sized 36px avatar retains its initial fallback;
name/email labels use plain text and elision. Uploaded profile images are rendered
through the bounded avatar service below. Static normalized presence and own status
labels now appear; manual status editing/reporting, profile navigation and editing
remain open.

[NativePreferences](../packages/native/src/native-preferences.cpp) stores the four
palettes, light/dark/system mode and English/Russian language per opaque owner hash,
separately from anonymous preferences. The language initially follows the system's
Russian/English language, with English fallback. The embedded reviewed Qt catalog
updates existing QML bindings without restarting and translates public C++ errors;
Qt's built-in translations load when present. The locale suite checks live
switching, scoped restoration, catalog completeness and argument preservation.
Picker-package labels deliberately remain English, matching the source web app's
unmodified emoji-picker-react configuration even when its app language is Russian.

Sidebar preferred width is global, defaults to 299 and validates
against 240–720 before viewport clamping. System mode follows Qt style hints.
Unknown stored values and invalid owner keys fall back safely. This settings file
has no credentials, server addresses, logins or user/project UUID fields. Electron
local-storage migration and the remaining settings are still open.

The global disk-cache setting defaults to **10 GB = 10,000,000,000 bytes** and accepts
whole values from 1 to 1000 GB. The production
[cache/preferences binding](../packages/native/src/native-app.cpp) applies it before
opening the cache and on committed setting changes; cache failures are visible in
settings. A setting value is distinct from proof that a large existing cache has
finished shrinking; see the response-cache constraints below.

Emoji suggestion history is scoped to the same opaque owner. It retains at most
14 MRU entries and presents them by frequency, with MRU ties, as in the pinned
picker. Separate daily counters cover exactly the current and previous six UTC
dates. Only an exact current owner and known canonical catalog identity can update
these bounded counters. Picker clicks update suggestions immediately; quick-use
counts update only after the controller confirms a new local addition, not after
removal, rejection, uncertain replay or a previous owner's result. Owner changes
replace history before emitting appearance/language notifications, so synchronous
observers cannot read the previous owner's history during a theme change.

The same preference store can attach to the production `QWindow` to retain its
normal geometry and maximized state globally. Move/resize persistence is batched
for 350 ms and flushed on application exit. Minimized/maximized dimensions do not
overwrite normal bounds; removed monitors and malformed positions restore onto
an available screen. An actual staged Linux launch reproduced maximized-window
normalization after a second launch, then passed after explicit state restoration:
the same process/window returned maximized and retained its 1100×720 normal size.
Visible minimized/hidden regressions cover this case. Electron state migration,
Wayland and other window-manager/display-scaling combinations remain open.

Do not put credentials in command arguments, fixtures, source files or logs.

## Desktop integration

The Quick main window now runs under `QApplication` so the separate
[desktop adapter](../packages/native/src/native-desktop.cpp) can use Qt Widgets
for the operating-system tray menu. Messenger presentation remains Qt Quick.
The tray reuses the source normal/unread PNG assets. Its Messenger entry and
primary activation restore the existing window through the same preference-aware
activation path as a second launch. A close request hides the window only while
the OS reports an available tray and the icon is visible. Without that recovery
path, closing exits normally.

Tray Quit and the conventional desktop quit key request the same asynchronous
cache shutdown as normal closing. The main loop exits after the cache worker
finishes; a hidden window does not prevent an explicit quit. The shortcut also
retains Qt's platform-provided Quit bindings. The menu uses the application
locale and the current verified account's aggregate channel unread count.
Mail and Calendar entries remain explicitly unavailable until those sections
have their native implementation.

The [desktop component tests](../packages/native/tests/native-desktop-test.cpp)
cover close/restore, unavailable or disappearing tray, hidden-window quit,
normal/unread icons, unsupported entries and real key-event dispatch. They
simulate tray availability and do not establish real desktop-host registration,
tray activation, Linux distribution coverage or Windows/macOS acceptance.
The frozen V13 stage passed actual available-tray close/hide, second-launch
restoration and keyboard quit on the current Wayland host. Actual tray-icon click,
native menu activation and the unavailable-tray fallback on a real desktop remain
unaccepted; the component tests cover their application-side behavior.

For measured disk-cache costs and the remaining large-shrink performance gap,
see [cache benchmarks](NATIVE_DESKTOP_CACHE_BENCHMARK.md).

## Messaging, realtime and files

### Response cache

[NativeCache](../packages/native/src/native-cache.h) is a bounded SQLite response
cache, separate from authentication, credentials and mutation confirmation. It
accepts an owner only after verified session introspection; the partition hash
binds the canonical server origin, project and user. Async callers retain a lease
whose generation also changes across cache-service replacements. Explicit removal
of a verified saved account can erase its partition without exposing its rows or
switching the active owner.

The private `messenger-v1` cache directory uses mode 0700 on Linux and database,
WAL and shared-memory files use 0600. Symlinks, unrelated/future schemas and failed
SQLite operations fail closed to network behavior. Typed DTO whitelists exclude
IAM credentials and arbitrary nested fields; encoded encrypted/control payloads
remain encoded. The default global disk budget is **10 GB (10,000,000,000
bytes)**. Settings expose integer 1–1000 GB; the storage API accepts 1 MiB–1 TB.
The budget includes the database, WAL and shared-memory file, with global LRU
eviction across owners. There is no default page-count ceiling or 32/64 MiB
ceiling. Explicit small test policies can still limit payload bytes/entry count.
The 8 MiB response-entry ceiling remains; tiny physical budgets lower the accepted
entry size to leave journal space. Larger entries are skipped rather than truncated.
SQLite grows on demand and does not preallocate the configured capacity.

The SQLite page cache stays at 2 MiB and in-process fresh-page bookkeeping is
limited to 4096 keys without deleting the other disk pages. Schema v2 adds a
message-to-page index; v1 migration preserves saved rows and rebuilds this index.
Confirmed-read patches and canonical message projections visit only matching pages
in indexed batches of at most 128 keys. They retain raw encoded payloads, page
cursors and ages, and never stamp patched pages fresh. Cache totals
are derived from actual blob lengths once on open or an external data-version
change; indexed per-row deltas and LRU batches avoid whole-cache scans on ordinary
reads/writes/stats. A damaged `size` column cannot bypass either the payload or
physical limit. An external writer also clears warm trust. Storage failures are
optional and report safe text through `errorChanged`.

[NativeCacheService](../packages/native/src/native-cache-service.cpp) runs the
synchronous storage API on one serial worker. The GUI keeps only status snapshots
and owner/lease guards. Optional writes have an eight-job, 32 MiB compact-JSON
queue bound; superseded same-page writes coalesce. Canonical message projections
share this bound and coalesce only for the same owner, route, UUID and field mask.
Independent payload, star and reaction updates preserve their FIFO order. A hard
route invalidation drops matching queued snapshots/projections before its durable
erasure, while unrelated routes keep their admitted work. Required erasures have reserved
admission separate from the bounded ordinary control queue, so a full read-patch
queue cannot silently discard an accepted account cleanup. Closing, changing owner
or superseding maintenance cancels storage work between bounded SQL/row/vacuum
steps. Shutdown stops admission, finishes accepted bounded writes and required
erasures, then closes the worker's SQL connection. The application keeps its event
loop alive until `shutdownFinished`; worker threads are never detached.

An explicit saved-account removal waits for successful exact-owner disk cleanup
before removing its credential/card. A failed, destroyed or replaced service keeps
the saved account and clears the pending UI state. Sign-out revokes authentication
immediately and queues required cleanup before clearing the identity; a later
cleanup failure is reported separately. Acceptance of that queued operation is not
proof of completed disk erasure. Erasing an inactive account through an already
open store preserves the active owner's warm trust.

Setting a limit accepts a request; it does not claim the physical change has
finished. Requested and applied values, availability, usage and maintenance state
are separate. A live reduction or cold open with a lower limit must finish LRU
deletion, checkpoint and any remaining physical reclamation before publishing
that applied limit.
Reads bypass unavailable maintenance instead of waiting behind it. Controller
navigation starts authoritative HTTP immediately during cold open or resize. If
messenger HTTP fails before disk open finishes, one pending hydration per visible
collection can still show saved rows when the worker becomes ready, without a
second automatic HTTP attempt. Late disk replies cannot replace newer REST or
realtime results, another route, account, owner lease or replacement service. A
cache-only revision change during a still-current warm lookup falls back to HTTP
rather than leaving that collection empty. Ordinary usage notifications do not
restart hydration or create request loops.

Controller and real-window regressions exercise warm navigation, delayed opening,
failed messenger revalidation, worker replacement and two-process restoration.
Standalone worker and production preferences-binding checks cover their separate
contracts; these local fixtures do not establish production or packaged offline
acceptance.
During reduction the old allocation and a bounded transaction journal coexist
until vacuum completes; the new final bound is not an assertion of zero temporary
peak. `sampledPeakDiskBytes` records post-commit samples before explicit
checkpointing and does not observe every internal SQLite write. The 1 MiB
regression samples database+WAL+SHM, including repeated LRU updates from cache hits.

During reduction, a store with a small existing freelist temporarily enables FULL
auto-vacuum before LRU eviction and restores INCREMENTAL afterwards. Each bounded
DELETE therefore reclaims its free pages before they accumulate into a large
freelist. The initial free allocation must fit within `entryLimitBytes()` (at most
8 MiB, smaller for tiny budgets), avoiding a large opaque automatic compaction.
Cancellation after an actual deletion, restart, newest-page preservation and
restored normal mode have regression coverage. The resource regression measured
9,240,576 accumulated free bytes in the predecessor and zero with this path; it
does not assert a wall-clock speed threshold. Normal reads/writes keep the
existing 4 KiB file layout and 2 MiB SQLite page cache.

Already large freelists retain the cancellation-aware fallback. It uses at most
256 explicit single-page vacuum steps per
transaction, also scaled down for small budgets, with cancellation checks between
steps. QSQLITE treats SQLite's zero-column vacuum rows as a non-select result, so
`incremental_vacuum(256)` followed by `QSqlQuery::next()` reclaimed only one page
in the measured driver. Checkpointing that one-page operation caused excessive
I/O. The predecessor resize regression recorded 2488 successful checkpoints before
that fix and 22 with bounded transactions, while preserving the newest page and final
physical limit. A separate cancellation regression reopens the interrupted
database and finishes its reduction without losing that page.

Rows written in the current verified session have a 120-second freshness window.
Reopening or disconnecting clears that in-memory freshness, so cached rows can
hydrate immediately but require server revalidation. Route invalidation removes
all pagination variants of a removed conversation/message. Confirmed local and
realtime read flags retain stale saved pages. Verified edit, star and reaction
GET responses now project only their owned fields into saved raw history; a
verified send can insert into an existing head page, capped at 50 rows. It never
manufactures a missing page or replaces an existing row from an older send
confirmation. These writes add no HTTP requests. Raw ciphertext and control
payloads remain intact, including during independent star/read/reaction changes.
Normal message events project their validated DTOs; ordinary channel/topic catalog
updates retain stale snapshots. Deletion, archive and changed channel access or
encryption still erase the affected cached data.

The controller fences canonical callbacks by API generation, cache service/lease,
route and monotonic revisions for each changed message field. Revisions are kept
only for the loaded window and active mutation intents, so A→B→A events reject a
delayed B confirmation without retaining an unbounded event UUID registry. A known
deletion keeps a tombstone through active confirmations; deletion also removes
queued matching cache refills/projections. A response cache remains separate from
the realtime cursor: cursors stay in memory, and every mutation still requires
authoritative confirmation. Retained pages display as stale and cannot grant
permission to send, react, upload or paginate while revalidation is incomplete.
The controller reuses verified warm pages and stales affected routes after normal
mutations. Four visible topic selections across two conversations
make two history GETs; two stream selections make one topic-list GET in the
synthetic window regression after read-marker traffic has settled. These counts
cover navigation GETs, not authentication, realtime polling or read-marker requests.
They do not establish live-service performance acceptance.

Saved rows display under the existing chat-loading pattern: a small top-left
status chip while updating, then the compact error/retry flow on failure. Reads,
uploads and server mutations remain unavailable until catalog, topic, history and
realtime trust checks pass. Draft composition stays local. An open message editor
retains its text during revalidation and disables Save; owner or route changes
close it. This stricter mutation boundary avoids trusting stale encryption/ACL
metadata.

A real two-process window regression writes history through HTTP in the first
process, then hydrates the persisted rows before delayed history GETs in the
second. It covers failed revalidation/retry, updated/deleted rows, existing-row
reaction readiness notifications and identity replacement. Both processes use
verified synthetic IAM responses; fully offline authentication, production
cross-client reconnect and packaged offline restoration remain separate acceptance.

Run the focused storage regressions with `ctest --test-dir packages/native/build
-R '^native-native-cache$' --output-on-failure`. They also report read/write times
for a synthetic 50-message page. A 4352-page case verifies zero whole-cache scans
after opening, the 4096-key freshness bound and a real queued Qt event-loop
put/get; it reports median/p95 write and event-loop times. Scaled regressions
retain more than 512 pages and 64 MiB, reduce/reopen physical limits, test actual
WAL growth from hits and preserve explicit tiny-policy checks. These measurements
deliberately have no flaky wall-clock pass threshold. Actual default-capacity
fill/RSS/shrink acceptance is reported separately from these scaled fixtures.
The `native-native-cache-service` and `native-native-cache-preferences` targets
cover worker responsiveness with a real SQLite lock, cancellation, bounded queue
admission, privacy erasures, callback lifetime/thread ownership, coherent snapshots
and the production requested/applied settings binding. Observer-deletion cases
cover every public status signal and queued completion with ASan/UBSan/LSan;
controller cleanup tests also verify that a destroyed service cannot leave a
saved-account card permanently busy or claim unconfirmed removal.

### Current messaging coverage

The preview reads channel/topic history and supports basic plain Markdown message
input, send verification, copying, quoting and editing/deleting own messages.
Starred metadata and its tested core method do not add an extra context-menu
affordance absent from the pinned source. The [native renderer](../packages/native/src/native-markdown.cpp)
uses MD4C's escaped GFM HTML with raw HTML disabled, then applies the existing
restricted Qt rich-text conversion. This keeps soft and hard line breaks inside
one paragraph; true paragraph boundaries retain their 4px gap. Fixed generated
task-list and strike tags are adapted for Qt, without enabling input HTML.
Raw message HTML does not execute, unsafe links lose their targets, and rendering
cannot fetch resources. Resource denial is installed before HTML import.
Secret-fragment labels/envelopes stay masked in visual and accessible message
text. Typed Workspace references preserve identity; file/image/video references
enter the save flow, while other navigation and media previews remain incomplete.
Encrypted message text is not exposed and plaintext sends to encrypted channels
are blocked. Full Markdown/URN/encryption interoperability remains unaccepted.

Bubble widths are measured from the sanitized rich fragment rather than plain
Markdown text. Eligible trailing root paragraphs reserve the 12px timestamp and
8px metadata gap inline; block tails and messages with reactions retain a footer.
Clock text is escaped before adding a controlled transparent reserve. Accessible
message names continue to use the renderer's masked plain output. Rich text and
Column layout are resolved for instantiated rows before restoring the viewport;
offscreen rows remain virtual. Source-component comparisons are synthetic scene
checks, not complete visual or live-service acceptance. Inline code styling,
shadows and the remaining content components still require matched checks.

[Realtime](../packages/native/src/realtime-client.cpp) establishes an epoch,
waits for the controller snapshot, catches up through paginated REST events and
uses WebSocket wakeups. Acknowledged cursors remain in memory and are scoped to
server/project/user. Generation changes, cursor expiry and reconnect have explicit
recovery paths. Selected message/topic/stream/read/user reducers exist; complete
event coverage, background accounts, durable cache/outbox and live cross-client
delivery remain open.

Read boundaries use the bottommost loaded bubble intersecting the actual message
viewport after a 250 ms batch. The scene requires an active, visible, nonminimized
window, a selected ready conversation and no blocking menu, dialog or picker. It
recomputes geometry at dispatch; cached offscreen delegates and the last model ID
are not substitutes for a visible anchor. The controller confirms the exact
server marker before updating loaded read flags and refreshing active/passive
counters. Initial unread positioning, durable reading positions and the remaining
read/notification workflows are still incomplete.

The scene reconciles verified message rows by UUID and preserves the first visible
bubble and its offset when incoming messages arrive while reading history or an
older page is prepended. Incoming messages follow the tail only when the view was
already within the source's 80px bottom threshold. A verified local send pins the
newly inserted row through the controller's exact completion signal; an own
message arriving from another client does not imply that local send intent.
Virtualized variable-height rows are laid out before read markers are considered.

Reaction chips use confirmed ownership metadata, native Unicode glyphs and the
source's spacing, corners and accent states. The 320×360
[picker](../packages/native/qml/NativeReactionPicker.qml) virtualizes the pinned
source categories and 1,895 eligible neutral bases, with search, skin selection,
500ms variations and a fourteen-entry suggested history. The core retains all
3,979 wire identities. Package labels deliberately remain English as in the source.
The bounded [quick capsule](../packages/native/qml/NativeQuickReactions.qml) shows
six base choices plus up to five popular identities from seven UTC days; only a
verified new local addition records quick usage. Suggestions record picker clicks.
Both histories use the existing opaque-owner preference scope.

Unknown provider names stay plain text; removal uses the exact confirmed own
reaction identity. Pending mutations disable repeated toggles, and an uncertain
result offers a confirmation-only status check. Picker acceptance is bound to its
original owner and conversation. Complete participant metadata displays overlapping
20px initial avatars with verified-directory names or a UUID-prefix fallback;
partial metadata retains the count. Names, tooltips and accessible labels cannot
interpret markup. Actual avatar resources, complete keyboard/accessibility parity
and matched visual acceptance remain open. Encrypted-message reactions use metadata
without exposing the body.

The source picker metadata and four unchanged SVG assets are generated from the
exact desktop-pinned official archive. Run the provenance check without executing
package code:

```sh
python3 packages/native/tools/generate-emoji-picker.py --archive /path/to/emoji-picker-react-4.19.1.tgz --check
```

The generator verifies the archive, lockfile integrity, individual source files and
wire catalog, then checks deterministic resources and the bundled MIT notice.
The build does not fetch an emoji image service or embed a React runtime.

A closed selected topic hides and disables the composer; its draft survives
verified realtime reopening. The source-style prompt opens a one-item menu, then
the verified reopen action performs one toggle POST and canonical topic GET.
Visible mouse/keyboard and failed-confirmation regressions check pending gating,
retained drafts and the absence of a repeated toggle after uncertain confirmation.

The same FileClient exposes a C++-only `downloadBytes` mode with a fixed 25 MiB
payload-length cap and no temporary disk destination. It first validates metadata,
then exact binary size and optional checksum through the same authenticated UUID
transport. A cold result costs one metadata plus one binary GET (the source avatar
loader uses a direct binary GET). Metadata failure cannot fall through to download.
Cancel, owner-generation replacement and deadlines complete once; destruction
suppresses completion. This is a per-operation payload bound, not a global RSS or
concurrency bound. The shared AvatarImages service bounds demand to 4096 leases,
64 retained 128px thumbnails, two network/decode payload slots and one serial
decoder. Its thumbnail map is at most 4 MiB; scratch/full decoded images and GPU
memory are separate. It publishes cache-only image-provider URLs and clears demand
on actual owner/API generation changes, without a new disk store or schema.

The actual Main binds this service to uploaded profile, Direct sidebar/header and
peer-message images, retaining existing initial/lock fallbacks and circle sizes.
The child releases its exact lease on reference/scope/visibility changes and
reads URLs only on revisions. It disables Qt's separate URL cache. Static PNG,
JPEG, GIF and WebP are supported; animation, external avatar URNs, complete loading
phase/failure recovery and full shell remain open. Static presence/view-only
profile labels are checked in [presence view checks](provenance/presence-main-checks.json);
manual editing/reporting and typing remain open. Local real-window
checks and raw matched Source captures are recorded in
[rendered-avatar checks](provenance/avatar-main-checks.json).

[FileClient](../packages/native/src/file-client.cpp) uploads nonempty files up to
25 MiB through authenticated multipart requests. It validates returned metadata,
streams downloads to an atomic local destination, and enforces size/checksum and
credential-generation boundaries. The composer chooses files through OpenFiles;
file/image/video links use SaveFile. Picker acceptance is bound to its original
owner and conversation, and save acceptance to its owner. Completion notices are
plain text. Preview/editing, numeric progress, cancellation/retry controls and
actual native picker/portal behavior require further work and acceptance.

Offline recovery, encryption, external providers, calls, notifications and the
remaining desktop integrations are tracked in the parity checklist.

## Verification

```sh
ctest --test-dir packages/native/build --output-on-failure
/usr/lib/qt6/bin/qmllint packages/native/qml/*.qml
git diff --check
```

Tests use synthetic fixtures. Physical HTTP tests permit only loopback addresses
through an explicit C++ test constructor option; the application uses HTTPS-only
defaults. Saved-session tests inject a narrow HTTPS request executor and fake
credential backend without relaxing production TLS or writing real OS secrets.
The vault tests check key ownership, disabled plaintext fallback, serialized
save/forget and rotation. Preferences tests recreate the settings object and
exercise Qt style-hint changes and separate owner restoration.

An optional platform check uses the actual operating-system secure credential
store. It creates only a unique disposable test key in the application's service,
sends random test bytes to child processes through stdin, verifies process-restart
read and rotation, and deletes the key with a final absence check. It never uses
Workspace credentials. Enable it explicitly in a separate build directory:

```sh
cmake -S packages/native -B packages/native/build-platform -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DWORKSPACE_NATIVE_PLATFORM_TESTS=ON
cmake --build packages/native/build-platform --target secure-store-platform-test --parallel 4
ctest --test-dir packages/native/build-platform -R '^native-secure-store-platform$' --output-on-failure
```

The regular suite uses fake stores and does not require platform permissions.
The optional check must fail if access is unavailable or denied; it cannot silently
substitute a plaintext or in-memory backend. An unlocked-store pass does not prove
locked-store, user-denial, device-restart or live session-restoration behavior.

The first real-store run on the current Linux desktop failed its initial
absence/read preflight with the backend's generic failure classification. There
was no active Secret Service owner, and no test credential write was attempted.
This remains a failed platform check; synthetic vault tests do not override it.

A separate run passed against a real GNOME Keyring 50.0 Secret Service on a
private session bus with disposable credential data. The same production
QtKeychain backend saved bytes, read them in a fresh process, rotated them,
and confirmed deletion. The private service stopped and its data directory was
removed. This confirms that isolated Linux provider path, not the current desktop
session, locked/denied behavior, live Workspace restoration or Windows/macOS stores.

The [instance coordinator](../packages/native/src/native-instance.cpp) uses a
long-lived lock and a local activation socket inside a private preview directory.
A second launch confirms activation of the running instance and exits; it never
replaces a live owner's socket. The socket accepts only a bounded activation
frame. [Process tests](../packages/native/tests/native-instance-test.cpp) cover a
second process, crashed-owner recovery, independent namespaces, malformed traffic,
unavailable directories and a fragmented acknowledgement. The installed current
Linux X11 window also passed minimize/second-launch activation with one surviving
primary process, maximization preserved and clean process exit. URI delivery,
native permissions, other Linux sessions and other operating systems still need
separate acceptance.

The window test opens an actual Qt Quick window. It covers sign-in, project
selection, navigation, send/read-back, remembered-account controls, theme/sidebar
restoration, verified profile metadata, nonblocking credential warnings and masked
accessible names. A two-window scenario checks that inactive history loading and
scrolling, hidden windows and minimization do not post read markers; focus and
visibility restoration advance only the actual visible boundary. File
tests use Qt's visible fallback dialog to choose/upload and save a synthetic file
through real local HTTP transfers. The test executable deliberately disables
platform-native dialogs for deterministic automation; actual OS picker/portal
acceptance remains separate. No test needs production credentials or messages.

Wheel-driven variable-height scenarios check incoming messages while reading
history, an own message arriving through realtime, older-page prepends, tail
following and exact verified local-send behavior. Runtime QML type/reference
warnings fail the window tests rather than being accepted beside passing actions.
Reaction scenarios exercise real local HTTP add/remove, unknown-provider ownership,
uncertain-result checking, encrypted metadata and late picker acceptance after
navigation or sign-out. They also bound the number of actual emoji delegates.
Mouse and keyboard focus checks verify that the profile ring follows keyboard
focus while mouse activation does not leave a ring. Matched component regressions
check bubble width/wrapping, inline metadata, fixed 40px picker cells, the two
1px border insets, the parent context menu remaining open and the absence of a
non-source emoji hover tooltip. Linux picker labels resolve to Liberation Sans;
the source's 30px Noto Color Emoji rule and Qt's calibrated 36px rule paint the
first grinning face at 32×31 pixels in their original captures at scale 1. Other
OS rasterization remains unmeasured. Header/history origin and 16px top padding
are checked independently of fixture wrapper coordinates; peer initial avatars
use the source primary color, 18px semibold text and 48px frame. The
trailing-paragraph boundary covers lists, quotes, fences, tables, headings and a
paragraph following earlier blocks; a hostile clock string cannot introduce HTML.

To capture that synthetic native window without starting a persistent test server:

```sh
CASSI_NATIVE_TEST_SCREENSHOT=/absolute/path/native-preview.png \
  CASSI_NATIVE_TEST_DARK_SCREENSHOT=/absolute/path/native-preview-dark.png \
  packages/native/build/native-window-test realQuickWindowSignsInNavigatesAndSends
```

Both screenshot variables are optional. The dark capture uses the visible account
menu to change appearance, waits for the actual theme change and closes the menu.
Tests inject temporary preferences so captures do not change user settings.
Optional `CASSI_NATIVE_TEST_METRICS` writes dimensions and resolved font information
for the same synthetic message scene to a JSON file.
Optional `CASSI_NATIVE_TEST_REACTION_SCREENSHOT` and
`CASSI_NATIVE_TEST_REACTION_DARK_SCREENSHOT` paths capture the same synthetic scene
with the 320×360 reaction picker open, before the hostile-label renewal fixture.
Optional `CASSI_NATIVE_TEST_MENU_SCREENSHOT` and
`CASSI_NATIVE_TEST_MENU_DARK_SCREENSHOT` capture the message context menu before
opening the full picker. They use the same synthetic identity and message data.
The `savedHistorySurvivesProcessRestartAndRevalidatesBeforeActions` scenario also
accepts `CASSI_NATIVE_TEST_CACHE_SCREENSHOT` and
`CASSI_NATIVE_TEST_CACHE_OFFLINE_SCREENSHOT` paths for saved/updating and failed
revalidation captures. `CASSI_NATIVE_TEST_CACHE_LOG_DIRECTORY` preserves each
child process's QtTest output in an existing artifact directory.

These captures are test artifacts and are not live-service acceptance. Compare them with
the existing client using the same content, viewport, locale, palette, scale and
state before evaluating visual fidelity. Keep run screenshots and logs outside
the product source tree.

The service-readiness UI regression holds an actual HTTPS history response and
admits live messages through the public controller API during its real ready
publication. It checks accepted history, its consumed revision, queued viewport
restoration, closed initial history and the visible new live body separately.
Semantic initialization does not wait for layout completion. The current full
UI138 result and its narrower transport boundary are recorded in
[semantic readiness checks](provenance/service-semantic-readiness-checks.json).


## Profile geometry regression

The synthetic three-line profile at 1440x900/DPR1 has a 76.5px top bar,
59.5px profile control at y8, 20/13.75/13.75px name/status/email line boxes and
36px avatar at y19.75. The control explicitly sets 6px horizontal/vertical
padding; a style's horizontal padding otherwise overrides the generic padding
and clips the email. Plain Row/Column positioners and unrounded center anchors
retain these fractional values. Actual independent height and truncation REDs,
the preserved first fractional-center failure and the unchanged passing oracles
are in [profile geometry checks](provenance/profile-geometry-checks.json).
The full Window237 passed on the visible Wayland/default OpenGL desktop.
Font glyph width/raster, body wrapping, full shell/runtime state and platform
acceptance remain separate work.
