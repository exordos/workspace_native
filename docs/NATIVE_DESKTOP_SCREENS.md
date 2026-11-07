# Native desktop screens

This is the implementation and acceptance plan for native screens beyond the
initial chat window. It does not mark these screens implemented or accepted.
The required result remains complete desktop functionality, an almost pixel-perfect
design, and full testing on Windows, macOS, and Linux.

The source baseline, revision, palette/geometry contract, platform matrix, and
overall gaps are recorded in [Native desktop parity](NATIVE_DESKTOP_PARITY.md).
The native application uses a single Qt Quick window with the C++ session,
transport, and domain layer. The current implementation boundaries below are
separate from the remaining screen and state acceptance checklists.

## Full-frame reference and next geometry acceptance

The pinned React reference now composes the actual
[app shell](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/widgets/layout/layout-app-shell.ui.tsx),
[TopBar](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/widgets/top-bar/top-bar.ui.tsx),
[Workspace sidebar](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/widgets/sidebar/sidebar-workspace.ui.tsx),
[folder rail](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/widgets/folder-rail/folder-rail-vertical-view.ui.tsx),
[chat header](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/widgets/chat-view/chat-header-channel.ui.tsx),
[message list](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/widgets/workspace-message-list/workspace-message-list.ui.tsx)
and [composer section](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/pages/chat/chat-page-composer-section.ui.tsx).
Light and dark captures use identical synthetic message/account/directory data at
1440×900, scale 1, English, Blue Cold. All 1,682 extracted source files are hash
verified against the baseline, and the 25 direct runtime dependency versions match.
The local scene imports the actual app stylesheet; no component geometry is
overridden to resemble the native result. A local request allowlist, blocked
WebSockets and a restrictive content policy prevent corporate or external
service access. This is a source-component reference, not live Workspace or
Electron-shell acceptance.

The full frame exposes differences that the earlier bubble-only reference could
not measure. With a two-line profile, the source toolbar is 64px plus a 1px bottom
border, followed by the 4px body gap; macOS has its separate 28px titlebar strip.
The message list places its 34px sticky date wrapper and an 8px group gap inside
its 16px top padding. The compact sidebar has a 32px Find field at x12/y16,
a separate 32px create action, a six-slot activity row, 60px stream rows and
56px ordinary topic rows. A stream description is not a last-message preview.
Directory data takes precedence over IAM fallback names in the source profile.
These measurements drive separate native geometry regressions. Their reference
capture does not mark native full-frame fidelity achieved. Remaining unsupported sidebar
activity routes remain explicitly disabled; their source positions
do not make those flows implemented. The narrow channel-context topic flow is
described below. Preview content, member/presence counts and the remaining
sidebar modes still need work. The profile now uses a verified project directory
name before the verified IAM fallback, matching the source caller.

Remaining matched-frame checks include source shadows, selected-text/action flows,
and non-square SVG sizing. For example, the original Linux light captures measure
the organization chevron's painted bounds as 13×6px in Chromium and 15×15px in Qt;
that is an open rendering defect requiring an actual visible RED/GREEN regression,
not a reason to change the shared body font. Disabled native preview routes also
have intentionally unavailable states until their real flows exist. The visible
harness now shares the production `QApplication` constructor: this Linux fixture
retained Ubuntu Sans and identical bubble geometry after replacing `QGuiApplication`.
Packaged launcher checks and other operating systems remain separate acceptance.

The full source caller supplies Reply, Forward, Copy text, Copy message link and
Select for a readable ordinary message; verified own messages add Edit/Delete.
Add reply additionally requires an active reply session. Image/link context adds
its own copy actions. The pinned message menu has no Star affordance: its starred
DTO and activity filter do not establish a mutation control. The extra native
Star row has been removed, with a visible regression requiring its absence;
the separately tested core method remains outside this context menu.

## Open Linux acceptance finding

One frozen integrated V17 run failed
`sidebarTopicCreationUsesCanonicalHttpAndScopedRecovery(unselected)` before the
name field appeared: 135 window cases passed and one failed. The original source
and failed log remain retained. One separate test-only instrumented Release run
using the same Wayland/software baseline passed all 136 cases. At its unselected
click, the menu was open and enabled, the expected target and route were retained,
the action belonged to the same active window, and press/release/click/trigger
signals were each delivered once. That observation did not reproduce or explain
the original failure. Its cause remains **OPEN**; later passing builds do not
establish a fix. A later V18 whole-window run reproduced the same missing-form
symptom in the `astral-128` row: 167 cases passed and one failed. The selected menu
action was enabled at the pre-click snapshot, but all five press/release/cancel/
click/trigger counters remained zero. The helper waits before dispatch, so that
snapshot does not establish the immediate input-time state or a cause. Its failure
screenshot was not written because the configured output directory did not exist;
the textual witness is retained. Failure-only diagnostics record target, route,
access witness, window, coordinates and action signals.

A later failure-only ring reproduced the V18 press at y=226 outside the popup's
then-current y=203..213 rectangle, although its action row was already 36px high.
The first rendered frame in a controlled probe had the complete 46px popup; no
10px displayed frame was proven. The test helper now waits for natural full row
layout and containment before its existing real mouse click. An empty-guard
control reproduced failure at 10px; the guarded version passed at 46px. One
original-order prefix passed all 98 cases. No product menu-height change was
needed. The original V17 failure remains retained without an immediate input
trace. The subsequent frozen V18c full run passed all 171 window cases and all
20 regular groups in 586.986 seconds. This is local synthetic acceptance; full
pixel/feature and live-account acceptance remain open.

The seventeenth comparison also corrects the direct-chat sidebar caption: a late
user-directory response and a realtime rename now notify the actual stream-list
model. Both visible regressions first failed, then passed; the separate controller
run passed all 612 cases. Eight real OpenGL captures keep the same dialog,
selector, input and dropdown rectangles in English/Russian and light/dark themes.
Their full-shell/font/shadow differences remain outside this component result.

The next Archive reference was measured separately in actual pinned source
components with synthetic Cassi data, using a fresh visible global Playwright
profile and blocked service/API/WebSocket traffic. At 1440×900, its existing
dialog is `(272,194,896,512)`, search field `(289,301,862,40.84375)` and fixed list
`(289,353.84375,862,240)`. A row is 51.703125px high: 20px name, 15.703125px secondary
label and 8px vertical padding on each side. The separate restore button is
32×32 with a 16×16 source icon; a vertical scrollbar reduces the available row
width. English/Russian light/dark normal and no-match states retain the same
dialog/list geometry; Russian references also cover 700px and 600px viewports.
Seventeen relevant pinned source files matched the audited input hashes. The
profile and local server were removed. These source-only captures do not accept
native Archive rendering, pending/error states, unarchive actions or live service
behavior; those remain required checks for the next feature.

Separate source-only English/light and Russian/dark references exercise the
actual unarchive handler with intercepted, empty-body loopback POSTs. During
pending, only the 32px restore control is disabled and replaced by a spinner;
Open history stays enabled. A synthetic 503 returns the restore icon and displays
the source inline error. That error expands the centered dialog to 515.84375px
high at y=192.078125; the 38px alert starts at y=351.921875 and the unchanged 240px
list moves to y=401.921875. The fixture's normal no-network policy remains intact;
only this isolated transport fixture permits intercepted same-origin requests.
No real service request was sent, and its profile/server were cleaned. These
references establish source visual states, not native or backend acceptance.

Ten additional source-only English/light and Russian/dark frames distinguish
catalog loading and failure with either no rows or retained rows, plus a one-row
list. Empty loading shows the source spinner/status in an 84px-high block at the
top of the 240px list; retained rows remain visible while its busy flag is true.
Catalog failure uses a plain 20px alert and moves the list to y=385.84375 while
the dialog remains 512px high. It suppresses the false no-chats empty state.
Without overflow a row is 860px wide and its Open button 820px; the five-row
overflow reference is 850px/810px. Reserve scrollbar width only when needed.
These references use unchanged source components and synthetic catalog state,
with zero attempted API/external traffic. Their visible browser profile and local
server were removed; they do not establish native or service acceptance.

The eighteenth comparison publishes the normal source/native pairs from a
separate private Archive candidate over the corrected archive core; it is not
yet part of the shared V18c source below. Its capture set contains four actual
Wayland/OpenGL frames covering English/light and Russian/dark normal and no-match
states at scale 1. The normal card, search, roster and row rectangles
match the source measurements above. The focused geometry/search run passed
11 cases, including 700/600px and a virtualized 505-row list, without extra
catalog/history requests from filtering. A real ordinary archived-topic composer
failure was reproduced and corrected; the encrypted-topic negative control stays
disabled. These preliminary frames retain the visibly different gray scrollbar,
caret/rasterization, shadow and existing shell/profile details. The complete
window run and final painted-control comparison remain pending. Original PNG
bytes and production-input hashes are retained separately; this is component
progress, not full feature, pixel or live-service acceptance.

The nineteenth comparison deliberately shows the later English/light pending
state before its indicator correction. The native scrollbar now uses the
measured source width/end regions and palette, but Qt Basic's small
BusyIndicator leaves the restore button nearly blank. The actual pinned
[small spinner](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/shared/ui/loading/spinner.ui.tsx) is a
14×14 ring with a 2px border; the previous 16px expectation was derived rather
than measured. Four rendered native regressions reproduce that size mismatch in
pending/loading and both themes. The corresponding final2 whole-window run was
stopped as superseded and is explicitly **NOT COMPLETED**, not accepted. Original
frames and failed inputs are preserved while the Archive-only indicator is fixed.

The twentieth comparison uses a new frozen private candidate after that fix:
four source/native pairs at 1440×900 show English/light and Russian/dark normal
lists, English/light pending restoration and the Russian/dark restore error.
The local 14px ring and scrollbar now follow the measured dimensions/palette.
The actual Wayland/OpenGL capture run passed 12 cases and produced 16 raw PNGs;
the parent reviewed the eight published frames and verified their bytes and
synthetic content before upload and UUID read-back. Card/search/roster/alert
rectangles match the source measurements; font and icon rasterization, shadows
and the surrounding shell/profile still differ.

The corresponding complete native window run passed 213 cases in 197.9 seconds,
with no changed individual waits. A separate six-row regression first reproduced
repeated model-property reads in the encryption lookup: 1011 reads per 505-row
catalog. The corrected helper snapshots each needed catalog once and reads
neither catalog for an empty selection; Boolean results are preserved. This
demonstrates fewer model-boundary accesses, not a measured CPU/latency improvement.
The earlier 166-case/260-second run remains explicitly NOT COMPLETED and its
cause is open. These private UI inputs still use the accepted atomic Archive
core; later placement/cache/E2EE work, shared integration, installed-package and
live-service acceptance are separate gates.

## Current native implementation boundary

The [sign-in scene](../packages/native/qml/Main.qml) now shows real saved-account
metadata with project, login and HTTPS organization labels, Continue and Remove.
The [controller](../packages/native/src/native-controller.h) exposes no credential
values. [Session](../packages/native/src/session.cpp) resumes through refresh and
user/project introspection; [the vault](../packages/native/src/session-vault.cpp)
stores refresh credentials in the OS keychain with plaintext fallback disabled.
Passwords and one-time codes are cleared from the form after submission and are
not persisted. Pending restoration disables conflicting controls; a denied or
unavailable secure store leaves the saved entry available for retry. Sign-out and
Remove delete only the selected saved identity.

The authenticated profile trigger uses a verified public IAM whitelist and the
source 36px avatar inside a 48px trigger. Name/email labels render as plain text
with elision; the initial avatar is a fallback, with no invented online status or
remote image load. Session replacement and sign-out clear the previous profile.
Profile drawers, avatar fetching, status and editing remain open.

The [visible-window regression](../packages/native/tests/native-window-test.cpp)
uses a synthetic injected secure store and HTTPS request boundary. It covers
restoration failure/retry, disabled states, identity verification, credential
rotation, visible sign-out and removal; this is not actual OS secure-store or live
server acceptance. These controls remain **IN PROGRESS**: authenticated account
switching, background accounts, session migration, complete desktop geometry,
translations and all three platform acceptance requirements are still open.

The same initial scene includes restricted native Markdown formatting and live
transport state. Basic file references enter an authenticated save dialog, and the
composer can choose local attachments for upload. The visible automated flow uses
Qt's fallback file dialog; native OS picker/portal acceptance, previews, editing,
progress and cancellation/retry controls remain open. Full Workspace reference
navigation, media, persistent chat state and complete realtime event behavior
remain open, as described in
[the implementation boundaries](NATIVE_DESKTOP_PARITY.md#initial-session-rendering-and-realtime-boundaries).

The scene now supplies the actual loaded bubble UUIDs intersecting the active
visible viewport, bounded to 256 per evaluation. The core confirms the latest
visible boundary separately for each actual topic; a mixed channel feed cannot
use one global bottommost message as a read boundary. Inactive, hidden or minimized
windows and open menus/dialogs/pickers do not create viewport read intents.
Two-window regressions check real focus changes and leave later offscreen rows
unread. Initial unread positioning, persistent scroll anchors, explicit bulk
topic/stream read and notification behavior still need implementation/acceptance.

Incoming appends preserve the visible UUID/offset while the user reads history;
older-page prepends preserve the same anchor. The view follows incoming messages
when already near the bottom and pins a verified local send when its row appears.
An own message from another client keeps the reading position. Wheel-driven
variable-height window regressions cover these behaviors; durable route positions,
initial unread opening and outbox behavior remain open.

Saved history now remains visible under a small top-left updating/failed-refresh
status chip and the existing compact error/retry flow. The presentation follows
[the source chat section](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/pages/chat/chat-page-workspace-message-list-section.ui.tsx)
and [floating loading overlay](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/shared/ui/loading/floating-loading-overlay.tsx),
without a separate storage panel. Reads and mutations wait for authoritative
revalidation; an open editor retains its text with Save disabled. A two-process
visible regression covers disk restoration before delayed GETs, failure/retry,
updated/deleted rows, readiness changes and another verified identity. Warm visible
navigation makes two history GETs for four topic selections and one topic-list GET
for two stream selections. These fixtures require verified synthetic IAM responses;
fully offline authentication, production reconnect and resource-cost acceptance
remain open.

Reaction chips use verified ownership and native Unicode. The 320×360
[Qt picker](../packages/native/qml/NativeReactionPicker.qml) keeps the source
category order, search/navigation sprites, source-sized painted glyphs, six skin tones and
500ms variation opening. It starts with 1,895 eligible neutral bases; variants
preserve the pinned source mapping while all 3,979 wire identities remain in the
core catalog. Only visible grid delegates are created. Package labels remain
English, matching the desktop configuration even when the app locale is Russian.
Empty search results have an accessible announcement and no invented visible row.

The Linux matched reference resolves picker text independently of the app body:
Chromium uses Liberation Sans Bold for 16px category labels and Noto Color Emoji
for the package's 30px emoji rule. Qt uses the Arial alias resolving to Liberation
Sans for picker text and the installed source emoji stack for glyphs. Equal
nominal font sizes do not guarantee equal painted glyphs: the Linux Noto Color
Emoji rule is calibrated to 36 Qt pixels. At scale 1, the first grinning-face
colored ink measures 32×31 pixels in both original source and native captures,
inside unchanged 40×40 cells. A visible regression checks that measurement and
the category font. The parent context menu and its quick choices remain visible
while the full picker is open. Body font resolution stays unchanged; other OS
emoji rasterization still needs its own matched reference.

The ordinary chat header and history meet without a layout gap, matching the
[source page's header/section flow](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/pages/chat/chat-page-workspace.ui.tsx).
The [message list](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/widgets/workspace-message-list/workspace-message-list.ui.tsx)
provides the 16px top inset itself. A dated first group additionally has its
34px sticky date wrapper and 8px gap. Native date grouping uses the validated
creation timestamp and local calendar day; labels follow the source Today,
Yesterday, and localized month/day rule. Visible checks compare the actual native
header/list origin and date-group geometry, independently of a reference fixture's
outer origin.
Peer initial avatars use the shared source [Avatar](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/shared/ui/avatar/avatar.tsx)
48px frame and 18px semibold text. In the generated source stylesheet,
`text-text-primary` wins the equal-specificity `text-accent-soft` utility;
light/dark regressions check the resulting primary text color rather than the
sidebar sender color. Actual avatar images remain a separate acceptance item.

### Aggregate channel conversation

Selecting the channel row opens a distinct mixed-topic conversation. The header
shows the verified stream name. Each ordinary group uses actual author UUID,
topic UUID and local calendar day; equal display names do not merge identities.
Only its first bubble displays the peer author and optional topic name, or just
the topic name for owned messages. Blank or unavailable catalog names produce no
invented label. Labels render as PlainText and navigate using the actual topic
UUID, including when a name contains markup characters.

The [source grouping](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/widgets/workspace-message-list/workspace-message-list-grouping.lib.ts)
keeps the preceding topic across day boundaries. A topic divider appears only at
an actual topic-run transition, never before the first loaded row or merely for
a changed author/date. Known labels have the measured 24px divider; an unknown
name keeps the two lines in a 9px divider. First-only headers use 12px/16px type,
a 6px author/topic gap and source accent text. Compact peer corners are on the
left, while compact own corners are on the right. The existing flat ListView
remains virtual; a peer group's single initial avatar follows its visible end.
Service-message expansion and actual avatar loading remain separate gaps.

The ordinary aggregate footer follows the actual
[stream topic prompt](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/pages/chat/chat-page-stream-topic-prompt.ui.tsx),
not the default-topic resolver helper. Its 57px frame contains a horizontally
scrollable rail of 36px shortcuts, 6px inter-item spacing and at most 224px per
item. Source ordered topics, unread badges and struck-through closed names are
preserved. Closed topics remain selectable and lead to the existing resolved
topic prompt. No ordinary aggregate composer, upload or Reply action is exposed;
choosing a topic restores that topic's draft and composer. The native independent
Edit dialog is a deliberate difference from the source footer, with the core
retaining the edited message's actual topic.

Matched source component scenes cover English/Russian and light/dark at
1440×900, scale1, with identical synthetic UUIDs, names and seven mixed-topic
messages. A 700×600 source reference also establishes rail overflow and the
separate global New Chat modal's internal scroll/footer behavior. These captures
perform no service calls or creation submission. The aggregate UI work does not
implement that global four-tab dialog, membership, encryption or complete
desktop parity. Visible tests cover identity/date grouping, unknown labels,
source corners, mouse/keyboard topic activation, closed topic selection,
per-topic visible reads and scroll-anchor preservation. Full matched-frame,
packaged runtime and other operating systems retain separate acceptance.

The message context menu follows the pinned [menu presentation](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/widgets/workspace-message-list/workspace-message-bubble-menu.ui.tsx)
and shared [dropdown geometry](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/shared/ui/dropdown-menu/dropdown-menu.helpers.tsx):
242px outer width including its border, 6px radius, 4px vertical inset, a 45px
quick-reaction bar, 28px quick buttons with 2px gaps, 36px action rows and 20px
source SVG action icons. Its surface, text, icons and hover colors use semantic
theme tokens. Reply precedes Copy text; an owned readable message adds Edit/Delete
after a separator. Peer Edit/Delete rows are hidden, and locked message bodies do not
expose unsupported copy/reply/edit/delete controls. Menu Home/End/arrow/Return handling
skips hidden rows, and opening the full picker preserves its parent menu.

This is still a partial action matrix. The source caller can also provide Add
reply, Forward, Copy message link, Select and image/link-specific actions; those
flows are not implemented in the native scene. Selected-text copying, forward
snapshot edit restrictions and cryptographic key actions need separate acceptance.
The starred activity filter does not imply a Star context action: that extra
native row is absent, matching the complete pinned source caller. Menu shadows
and platform accessibility remain unaccepted.

The [quick capsule](../packages/native/qml/NativeQuickReactions.qml) uses the source
40px frame, 36px choices, 0.6 collapsed scale, 150/200ms hover timing and a
viewport-bound maximum height of 176px. Its at most eleven choices combine six
base reactions and five popular identities from seven UTC days, sorted by usage
with source tie ordering. Full-picker suggestions separately retain at most
fourteen recent identities and display frequency order. Suggestions record the
picker click; quick usage records only a confirmed new local addition. Both
histories use the existing opaque-owner preferences and reject late owner events.
Visible fixtures cover restoration, ownership clearing, mouse selection, capsule
Escape/arrow/Return navigation, picker search/Return and skin reset.
Linux visible ink checks cover the menu's source 15px emoji and the capsule's
source 24px emoji rules inside unchanged 28px and 36px hit areas. Their measured
Qt font rules use 18px and 28px respectively; the thumbs-up ink matches the source
menu's 14×16 pixels exactly and the capsule's 22×24 pixels within one pixel.
These measurements do not establish other glyphs or OS font fallbacks as identical.

Unknown provider names remain plain text and can remove only a verified own
reaction identity. Pending writes disable repeated toggles; uncertain outcomes
offer only a status check. Owner/conversation changes close the picker and reject
late acceptance. Encrypted-message metadata can carry reactions while the body
stays masked. Complete participant lists show 20px overlapping initial avatars
with verified-directory labels or `#uuid-prefix` fallback; incomplete lists keep
the count. Labels and tooltips use plain text, including markup-like names.
Actual avatar images, complete platform accessibility and matched visual
acceptance remain open. Profile and button focus rings follow keyboard-visible
focus; a real mouse/keyboard regression covers the profile trigger.

A resolved selected topic replaces the composer with the source-style 56px prompt.
The existing draft survives a verified realtime close/reopen. New sends and uploads
are blocked in the core as well as the scene. The underlined prompt opens the
source-style one-item menu before requesting reopening. The action stays disabled
during its POST and canonical topic confirmation; uncertain confirmation keeps the
topic closed and preserves the draft. Visible mouse, keyboard and failed-confirmation
regressions exercise the real HTTP action; full topic-management parity remains open.

The native channel row now exposes **New topic** through its right-click menu,
ContextMenu key and Shift+F10. Opening the context menu does not select the channel.
It follows the actual [sidebar caller and name dialog](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/widgets/sidebar/sidebar-workspace-context-menu.ui.tsx)
and [keyboard context helper](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/shared/ui/dropdown-menu/dropdown-context-menu.lib.tsx).
The global **New chat** control remains independent and now opens the ordinary
channel slice described below. The native channel menu currently implements this
one action; membership, notification,
folder, rename and archive actions from the full source menu remain open.

The centered name form follows the measured source scene: 448px card, 25px
outer content inset including border, 26px heading/close row, 16px spacing,
20px description and label, 6px label gap, 398×40.84375px input at 16px type,
and 36px footer buttons. At 1440×900 the ordinary card is 246.84375px high.
Close receives initial focus; Tab reaches the name and footer actions. Its focus
ring follows mouse versus keyboard activation, and disabled opacity applies to
the complete Create button, matching the source control.
The target, title, errors and labels use plain text. The card adds one bounded shadow layer on supported scenegraph backends; its
OpenGL fade is compared with the pinned source pixels. Qt
[software adaptation](https://doc.qt.io/qt-6.8/qtquick-visualcanvas-adaptations-software.html)
cannot render shader effects, so that backend keeps the complete plain card
without an enabled shader layer. A visible fallback regression covers the form
and real HTTP creation. Cross-platform glyph, shadow and decoration differences
still need matched review; these dimensions do not establish full pixel parity.

The [controller](../packages/native/src/native-controller.h) owns creation and
navigation. A single POST is followed by exact canonical topic verification and
a complete authoritative target-channel catalog before selection. Current and
nonselected channels share the same path; encrypted channels create metadata
without exposing message bodies. Unicode length is checked as 1–128 code points
in the core, without a UTF-16 field limit that would truncate emoji. Busy state
blocks repeated submission. An uncertain result retains its identity and exposes
only **Check status**, gated by the controller; reopening the form cannot repeat
POST. Cancel, close, outside click and Escape dismiss the form without undoing
an already submitted request. Owner, route, removed-channel or opaque ACL-context
changes close and clear old UI. The core independently rejects stale callbacks.

Inline errors and retained names are a deliberate native recovery extension:
the pinned source closes the name dialog and logs failures. Local visible HTTP
regressions cover context/keyboard entry, whitespace/Enter, current and
nonselected targets, denied and uncertain confirmation, dismiss/reopen while
pending, route/logout/ACL changes and Unicode limits. The corresponding pinned
source and native dialog scenes use English/Russian, both themes, 1440×900 and
scale 1. They contain synthetic data only, with external requests and WebSockets
blocked in the source scene; no live Workspace creation is accepted by them.

### Encrypted service-group disclosure

The native service-run slice now groups after ordinary/account filtering, renders
one localized 28px header and unmounts collapsed bodies. Real keyboard/click tests
cover initial accepted-history collapse, eligible live opening, manual state across
split/merge/head deletion and offscreen/history barriers. Controlled selection and
focus inputs have coverage; the complete user-facing entrypoints remain open.

Two dark Source component comparisons (English two-event and Russian five-event)
match the 1440 by 900/DPR 1/Moscow ordinary-row inputs. Header y/height agree, width
differs by less than 1px, and the following ordinary message has the source 16px gap.
The previous 12px native gap failed the same test before correction. Captures and
comparison messages retain explicit unmatched shell/profile/footer inputs; these
are collapsed-header/ordinary-layout checks rather than full-shell or expanded
crypto acceptance.

V33 adds owner-scoped bounded Body/indexed-History supplements and mounted
Grant/Reject lookup, with no visible R insertion or collapsed encrypted/getter
fetch. Disclosure serials distinguish genuine remount from routine re-render;
shared R, cache fallback and owner/runtime/realtime fences retain the existing UI
policies. Accepted V33 Body83, Service53, encrypted UI121, Controller739, Window220,
Runtime10, locale14 and Markdown21 remain baseline evidence. Its matched expanded
G/J comparisons use exact public DTOs and actual Source loader/correlation/IndexedDB
paths with a synthetic key/permission state adapter.

V34 adds ordinary/account public recipient lookup before relevance filtering and
grouping, including native and provider feeds without a Vault. Actual Main keeps
the foreign G/J hidden until its canonical R establishes the account recipient;
a ready-empty session then auto-opens the group. One real Space collapses and
unmounts it without another GET. Own-author relevance is preserved. These public
facts enable no ordinary/provider key action or encrypted sending; plain-message
policy is unchanged.
Interrupted lookup resumes after verified WSS catch-up; settled transport failures
still require a new semantic consumer rather than routine update retries. Existing
four-active/500-record/2,000,000-unit bounds and owner/access fences remain.

Two English/light Source37 cases match actual canonical R/GJ inputs at 1440 by
900, DPR1, with a null crypto session. The real account resolver starts its sole
GET while there is no group or body, then global-store/IndexedDB publication opens
the group; real Space unmounts it. Expanded title y/height match Native; horizontal
origins differ by less than 0.33px and widths by less than 0.66px. These narrow
checks do not match full shell, sidebar/profile/focus paint or participant counts
from unobserved bindings, and do not accept crypto issuance or a live account.

The V34 Core and complete encrypted UI suites pass 246 and 124 cases; focused
Main and capture groups pass 5 and 4 Qt results. Current Controller739, Window221,
Runtime10, locale14 and Markdown21 also pass. Cache83 and Service53 retain exact
applicable inputs. The staged package passes installed runtime checks and anonymous
Wayland startup, single-instance activation and quit. Complete selection/focus entrypoints,
ordinary/provider issuance and authenticated live acceptance remain open. See
[native crypto](NATIVE_DESKTOP_CRYPTO.md) for the current coverage and run commands.

### Global New chat: ordinary channel slice

The live Workspace [sidebar caller](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/widgets/sidebar/sidebar-workspace.ui.tsx)
passes four tabs: **Start chat**, **Create channel**, **Create topic**, and **Archive**.
The generic dialog's Browse tab is excluded by that caller. The native shell keeps
those four slots and enables Start chat, Create channel and Create topic. A fresh
idle opening starts on Start chat, as in the source. Archive remains explicitly
unavailable; channel-context topic creation remains a separate entry. Reopening
restores the last-viewed unresolved flow. If multiple flows have an unresolved attempt,
the local last-viewed flow wins; switching tabs never cancels or resets either
core identity.

The [QML form](../packages/native/qml/Main.qml) follows the measured pinned
[creation dialog](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/features/create-chat/create-chat-dialog.ui.tsx):
896px maximum card, 10px radius, 51px heading, 39px desktop tabs with 38px buttons,
16px horizontal body inset, 12px column gaps, 40.84375px single-line fields,
118px settings group, 160px member viewport, and a fixed 57px footer with 32px
buttons. At 1440×900 the ordinary card is 749.53125px high. At 700×600 it is
512px high; tabs become 26.015625px and the body scrolls independently of the footer.
Keyboard focus reveals fields inside that body. Close receives initial focus;
mouse and keyboard focus rings remain distinct. Enter in a field does not submit.
The intrinsic source checkbox's blue fill and tick are measured independently of
the application's palette accent. A small font-independent tick avoids relying on
platform checkmark glyphs. Shadow rendering keeps the same supported-backend and
software fallback boundary as topic creation. Focus comparisons wait until the
source CSS color transition reaches the accent border and the member-row hover
fade has completed; a merely focused input can still be in a transient visual
state. The focused search's opaque vertical border matches the source accent;
fractional horizontal edges and the blinking caret remain separate raster/dynamic
comparison limits, not evidence for changing the production palette.

Name and description stay intact until the core validates 255 and 10,000 Unicode
code points respectively; no UTF-16 field limit truncates supplementary characters.
Invite-only and creation announcement are independent flags. The separate
announcement-only option remains disabled, as in the source. Encrypted channel
creation stays disabled until its bootstrap flow is integrated. Ordinary creation
always sends explicit unencrypted metadata; this form does not enable key storage,
message encryption or remote key publication.

Member choices come from the verified current project directory, exclude the
viewer, and show plain display names and email labels. Search uses display name,
username and email, case insensitively. Selected users sort first, retain their UUIDs
through filtering and live label changes, and are never silently pruned when the
visible directory changes. A virtual ListView creates only visible member rows;
missing presence is omitted rather than invented. The matched source fixture currently
projects an offline dot, while this metadata-only native roster has no presence field;
its dot and the associated 16px name offset therefore remain a visible difference.
Creation with no selected members
does not require a successful directory load. Selected members require verification
and are submitted through the actual follow-up membership operation.

The [controller contract](../packages/native/src/native-controller.h) owns the
attempt identity, typed canonical/default-topic confirmation, initial member set,
authoritative catalogs and final aggregate-channel navigation. A pending attempt
disables duplicate writes and freezes the submitted fields. Cancel, close, outside
click and Escape dismiss the UI without cancelling the submitted POST. Reopening
recovers the same unresolved intent even if its earlier route context changed.
An uncertain outcome offers **Check status**, which performs reads only. A confirmed
channel missing its selected members offers **Add members** or **Retry members**;
these explicitly retry the same confirmed channel and frozen set rather than create
another channel. No optimistic stream, topic, member row or navigation is published.
The resulting ordinary channel opens the mixed-topic feed and topic-selection rail;
there is no implicit default-topic composer.

Owner/logout, route and real access-context changes close and clear old UI. Benign
unread counters and directory display-name updates do not close the form or remove
selection. Matching stream/default/binding creation events reconcile without a
manual Check. Deleting the confirmed channel allows only an explicit new attempt;
deleting its default topic retains the channel and GET-only recovery. A newer
default-topic event cannot publish an older saved catalog. The core also fences late
callbacks independently. Inline plain-text errors and explicit recovery actions are
native extensions to the source's close-and-log failure behavior.

[Visible window tests](../packages/native/tests/native-window-test.cpp) exercise the
ordinary HTTP paths, pending dismissal, same-stream member recovery, directory
filtering/virtualization, Unicode limits, keyboard visibility, modal read suppression
and direct realtime-event interleaving. Synthetic source/native scenes compare both
languages and themes without production writes. These checks do not establish live
Workspace creation, packaged creation, complete channel management, or Windows/macOS
acceptance. Full pixel parity remains open, including unsupported tabs, E2EE, presence,
platform shadow/raster differences and the surrounding application gaps.

### Global New chat: Create topic metadata slice

The third tab uses the existing topic-creation controller and the pinned source
[Topic panel](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/features/create-chat/create-chat-dialog.ui.tsx)
and [Select/Listbox](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/shared/ui/select/select.ui.tsx). It does
not turn the global New chat action into topic-only creation. Its local stream
selection and name survive tab changes and clear when the dialog closes. The list
contains every current non-archived stream, sorted by its raw channel name;
direct-chat peer labels do not replace that source label. The selected valid UUID
is retained, otherwise the first sorted option is selected. Transient busy/access
capabilities gate Create, without removing options or silently retargeting the
selection. Typed visible HTTP cases cover ordinary, direct, encrypted and provider
stream metadata; that does not enable encrypted message content or provider
publication acceptance.

At1440×900 the card is x272/y194 at896×512, with the Select trigger at
x289/y333 at862×40, the name field atx289/y417 at862×40.84375 and a fixed57px
footer. The trigger and option row use6px radii; the source focus ring adds2px
at20% accent opacity. The option panel has12px corners, a6px trigger gap,
5px inner inset and36px rows. Its width and maximum320px height are capped to
available viewport space with8px edge margins, choosing above only when below
is insufficient and above has more space. The list is virtual: a504-stream case
requires fewer than30 option delegates. The empty state retains its source
placeholder and empty-list text.

Close starts with focus. Arrow keys, Home/End while expanded, Enter/Space selection,
700ms prefix typeahead, Tab, focus transfer and Escape follow the source Select
behavior. Escape dismisses the dropdown before the modal and keeps trigger focus;
normal Enter in the topic name does not submit. Core validates128 Unicode code
points, without a UTF-16 field truncation. All channel names, field/error text and
accessible names remain plain text. Final source comparisons wait for the finite
chevron rotation transition to finish and retain its computed rotation and active
animation state; transient earlier frames are not a product arrow defect. Local
icon size requests preserve the non-square source chevron and check viewBoxes,
with visible pixel regressions for their15×8 and14×10 ink bounds. The option
panel reuses the existing hardware shadow effect and checks its actual fade;
software rendering disables that shader layer. The hardware approximation still
has small shadow/raster differences and does not establish full pixel parity.

Global and sidebar-context entries share one existing core intent. A request from
the sidebar does not force a fresh New chat opening onto Topic; explicitly viewing
Topic restores the same pending or uncertain UUID/name. Once Topic is the last
viewed unresolved flow, reopening restores it. Cancel/X/Escape only dismiss the
view. Uncertain state freezes submitted metadata and offers GET-only Check status,
without another POST. Owner, route, trust or actual target access changes close
and clear old UI, while harmless names/counters keep the UUID selection and draft.
Both views capture the exact admitted attempt UUID, independent lifetime context
and target access witness. Completion closes only the form bound to that exact
UUID. A controlled QML notifier-order regression starts and opens a second
same-target/name intent in an earlier listener; the first UUID completion must
leave that newer form open. This is a deterministic interleaving fixture, not a
claim that a spontaneous user race was observed.

This Linux source-component/UI slice remains separate from the open V17 menu
acceptance finding above, live Workspace writes, installed creation workflows,
Windows/macOS testing, Archive, E2EE bootstrap and remaining shell/presence/shadow
or raster fidelity work.

### Global New chat: ordinary Start chat slice

The initial **Start chat** tab uses the same 896×512px shell, with one search field,
a fixed 240px virtual roster and a Cancel-only normal footer. At1440×900, the
pinned source's card is x272/y194, search x289/y301 at862×40.84375, roster
x289/y353.84375 at862×240 and Cancel y661 at32px high. At700×600, the card is
x0/y44, search x17/y151; it remains512px high. The former y138.015625
expectation was derived from the Channel tab and was not a measured DM reference.
A new computed source control establishes 39px DM/Topic tabs at700px and 35px
at600px; font size changes from14px to12px at the source640px breakpoint. The
long Channel form independently compresses its tab strip under intrinsic flex
content pressure. A reversible local source probe changing only the middle
container's flex basis from0% to0px changes that strip from26.015625px to39px,
and restoring it restores26.015625px. Native tab layout follows that measured
weighted shrink rather than applying the Channel value to every tab. The600px
native test is a breakpoint control below the production700px minimum window.
Native geometry assertions use the measured source rectangles after layout settles. Search/filtering never
shrinks the roster or dialog. The source and native synthetic scenes cover both
languages and themes with the search focused after its accent transition settles.

Each whole user row is one button with a14px medium display name and an optional
11px secondary email. No checkbox, multi-selection, encryption toggle, name input
or additional normal Create button appears. Search uses trimmed case-insensitive
display name, username and email; a hidden username may match. DM rows sort by
display name, without the channel form's selected-first rule. Both tabs share one
local search string. Switching tabs preserves channel inputs and selected UUIDs.
The roster instantiates only visible rows; a502-user fixture checks that fewer
than30 row delegates exist. PlainText applies to user labels, accessible names
and scoped errors. Missing presence/status data is omitted, retaining the known
16px source dot/name-offset gap rather than manufacturing an offline state.

The [ordinary direct-chat core](NATIVE_DESKTOP_DIRECT_CHATS.md) owns pair identity,
GET-before-create lookup, typed stream/default/catalog confirmation and navigation.
UI delegates click/Enter/Space to that API and never computes an identifier or
constructs a stream/topic. Both creation flows share a write gate. While a request
is pending, duplicate and different-peer writes are disabled; the tabs and Cancel
remain usable. Dismissing the view does not cancel transport. Reopen restores the
same pending/uncertain attempt without a request. Owner/logout/route/access or
cache-trust changes close and clear old UI; benign directory label/counter changes
preserve it. An unverified directory says **User list is currently unavailable.**,
without inventing loading or connectivity status.

Recovery is an explicit native extension: uncertain attempts offer GET-only
**Check status**, with **Retry** only when the core permits one deliberate
canonical-pair ensure action. Repeating the same uncertain peer checks status;
choosing another peer is an explicit replacement when no mutation is pending.
Encrypted and archived existing pairs show scoped unsupported status with no
POST, downgrade or unarchive. Normal success opens the stream aggregate and its
choose-topic prompt. Composer/upload/reply remain blocked until a real topic is
selected. The partner header applies on aggregate and topic routes, using only
verified public metadata:40px initial avatar,56px shell and14px semibold name.
Presence, avatar download, profile/call actions and their source second-line
presentation remain separate gaps.

The [visible regressions](../packages/native/tests/native-window-test.cpp) exercise
real local HTTP with synthetic identities, row keyboard dispatch, existing-pair
zero-POST behavior, unknown outcomes and explicit recovery, unsupported pairs,
header/topic navigation, unavailable directory, pending dismiss/reopen, true
context loss, harmless label updates, PlainText, search and virtualization.
These fixtures do not establish live Workspace/primary-account, packaged creation,
full direct-chat capability or Windows/macOS acceptance. Full pixel parity remains
open for the two disabled tabs, presence/avatar/profile/call capabilities,
application shell, shadows and font raster differences. The final production-core
integration and all-platform acceptance remain independent gates.

[Native preferences](../packages/native/src/native-preferences.cpp) now persist
palette and light/dark/system mode per opaque owner hash, with separate anonymous
defaults and one global preferred sidebar width. The settings file has no server,
login, user UUID or credential fields. Invalid palette/mode/width and malformed
owner keys fall back safely. The [preference regression tests](../packages/native/tests/native-preferences-test.cpp)
cover owner restoration, fresh-object persistence and Qt appearance changes; a
visible-window regression changes the theme, drags the sidebar and recreates the
window/settings object. Full application restart, actual OS appearance changes,
Electron migration and the remaining settings screens are still unaccepted.

## Reference set and acceptance discipline

Use the actual desktop implementations linked below, the
[component catalog](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/docs/COMPONENT_CATALOG.md), and adjacent tests/stories as references.
Historical feature lists do not prove current Workspace behavior. In particular,
preserve explicit unsupported states instead of inventing a working operation.

### Matched source/native comparison plan

The next comparison must use the same synthetic message payloads, author labels,
conversation names, viewport, DPI, locale, palette, scroll anchor and interaction
state on both sides. Include normal history, own/peer reaction chips, the full
320×360 picker, saved-history loading and refresh failure. Freeze animation and
pointer position consistently; keep keyboard-focus and mouse-focus captures
distinct. Record which source build and native build supplied the images.

Browser work requires the global visible Playwright session, a fresh task profile
and verified primary `cassi` authentication against the explicitly selected
organization/project. Review the fixture/network guard before capture: synthetic
read responses must be confined to that task's browser context, and all messenger,
file and action mutations must be blocked before dispatch. Isolate realtime so
production events cannot replace the synthetic scene. Do not create production
messages, accounts or files to stage a picture. Keep credentials and production
content out of screenshots and artifacts. If the browser tooling cannot enforce
those boundaries, use a local source-component fixture and clearly identify it as
such; primary-account sign-in verification does not turn a local fixture into
live-service acceptance. This comparison is planned and has not passed acceptance.

### Source emoji picker data and next adaptation

The desktop uses `emoji-picker-react@4.19.1`, configured for native Unicode, search
and no preview in [the bubble menu](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/widgets/workspace-message-list/workspace-message-bubble-menu.ui.tsx).
It does not use EmojiMart. The pinned package supplies its ordered category and
emoji data under the MIT license. Its neutral catalog contains 1,898 base entries;
skin variants expand that to 3,773 entries, all represented in the existing native
3,979-entry backend catalog. The additional 206 backend entries do not acquire a
source-picker category by inference.

The compact [category/order/variant resource](../packages/native/resources/emoji_picker.json)
and [MIT notice](../packages/native/resources/emoji_picker.LICENSE) now preserve that
package's metadata. The [generator](../packages/native/tools/generate-emoji-picker.py)
verifies the official archive's SHA-256, desktop lockfile SHA-512 integrity, source
file hashes and backend-catalog hash. It reads exact archive members without
extracting paths or executing package code. `--check` verifies deterministic output.
The domain keeps all 3,979 wire identities and aliases, adds presentation fields
and validates each source glyph/base/variant binding. Its Unicode presentation
retains source variation selectors, including the red-heart glyph. No React or
remote image runtime is embedded. Four unchanged package SVG assets supply category,
search, clear and variation-pointer geometry. The visible adaptation uses the
existing virtualized Qt grid.
The source order is Suggested, Custom (hidden when empty), Smileys & People,
Animals & Nature, Food & Drink, Travel & Places, Activities, Objects, Symbols and
Flags. Its first neutral row starts with grinning face, grinning face with big
eyes, grinning face with smiling eyes and beaming face; Unicode sorting is not
equivalent. The picker uses 30px glyphs with 5px padding, category navigation,
search/clear and skin-tone controls. Three package-default failing symbols stay
hidden, so 1,895 neutral bases and 3,770 mapped identities are picker-eligible.
The remaining 206 wire-only identities retain their original behavior without an
invented source category. Skin-tone state starts neutral for each picker mount;
the source does not persist it. The native picker resets it on each opening.

The full picker and quick capsule have separate usage histories. The package's
`epr_suggested` list stores at most 14 entries and defaults to frequency order.
The Workspace [quick-reaction history](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/widgets/workspace-message-list/workspace-message-quick-reaction-history.lib.ts)
counts seven UTC days; [quick choices](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/widgets/workspace-message-list/workspace-message-quick-reactions.lib.ts)
combine six base choices with at most five additional popular identities, then
sort by usage with stable ties. Native persistence must remain optional and use
the existing opaque-owner preference scope. No production usage or Electron local
storage has been migrated. Fresh native usage is recorded separately through the
existing preferences object.

The existing visual review artifacts `reference-desktop-normal-masked.png` and
`native-quick-v1.png` establish the starting comparison. They use different message
content, and the desktop sidebar is masked. They cannot establish pixel parity for
text wrapping, sidebar rows, or the screens in this document. Keep these artifacts
as progress references; create matched synthetic reference/native pairs for each
new screen and state. Do not copy production data into screenshot fixtures.

The initial matched source-component baseline covers closed messages and the
default picker in both light and dark modes. It uses synthetic content and the
pinned source presentation components; it is not a full desktop-shell or live
service acceptance result. Remaining matched states below are still required.

Matched pairs must use identical synthetic names, UUIDs, message text,
reaction counts and complete/incomplete participant lists in the pinned desktop
source and native scene. Match the viewport, display scale, font availability,
locale, palette and light/dark state, then capture the closed picker, default
picker, search, category jump, skin variants and expanded quick capsule. Authenticate
the reference browser through the primary account, but keep fixture traffic local
and read-only: production messages, read markers, reactions and drafts must not be
modified to manufacture a screenshot. A synthetic pair establishes scene behavior
and geometry only; it does not establish production API or Electron integration.

Every functioning screen needs its complete entry/action/result flow, keyboard
operation, permission handling, and failures. A rendered form or an enabled button
does not complete a feature. Until its controller operation exists, hide or
explicitly disable the action; do not return invented success, generate domain
records locally, or show synthetic users/accounts in normal application models.

## Navigation and shared native presentation

The reference uses a layered 315px right drawer, rather than routing every account
screen into the message pane. Its history separates chat information, account,
and About layers. Within the account layer, the screens are root, settings,
appearance, About, and personal information. A profile opened from a message is
bound to its user UUID; encrypted-chat subpages are bound to owner, stream, and
runtime generation. Sources: [drawer state](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/widgets/right-panel/right-drawer.model.ts),
[drawer shell](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/widgets/right-panel/right-drawer.ui.tsx),
[layout integration](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/widgets/layout/layout-main-workspace.ui.tsx).

- [ ] Preserve Back, close-current-layer, collapse/reopen, and return-to-chat
  behavior. Opening account settings must not silently replace another user's
  profile or discard the previous chat-information layer.
- [ ] Clear or invalidate owner-bound history on account/project changes. Late
  profile, member, endpoint, or key results must not repopulate the previous drawer.
- [ ] Match a 315px drawer, 6px outer radius, shared header, inner scrolling, and
  edge-to-edge menu hover. Account rows are flat, with inset separators, rather than
  individually raised cards.
- [ ] Reuse a native menu-row component matching
  [PanelMenuRow](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/shared/ui/panel-menu-row/panel-menu-row.ui.tsx):
  16px root horizontal inset, 32px icon box, ordinary 22px glyph, 8px text gap,
  14px/20px title, and variant-specific subtitle. Compact rows are 40px high;
  regular rows expand for their content.
- [ ] Reuse a native dialog shell matching
  [AppDialog](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/shared/ui/app-dialog/app-dialog.ui.tsx): 50% black
  modal backdrop, semantic border/surface, 10px ordinary radius, consistent close,
  cancel/submit controls, focus trap, Escape policy, and focus restoration. Forms
  own their pending/dirty dismissal rules; not every dialog has the same footer.
- [ ] Localize labels in English/Russian, keep untrusted labels plain/sanitized,
  preserve Unicode/IME and text selection, and provide accessible names and roles.

Suggested native presentation components are an account drawer with a small screen
stack, a common row and option surface, a profile view/editor, and reusable modal
shells. Keep API actions and durable ownership in the existing C++ core. Add
screen-specific view models only where their state cannot fit the existing owner.

## Account root, settings, and appearance

Reference: [account menu](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/widgets/right-panel/right-panel-user-menu.ui.tsx),
[settings/appearance](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/widgets/right-panel/right-panel-user-settings.ui.tsx),
[settings contract](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/features/settings/settings.types.ts).

The account root groups current server/account, external accounts, personal
information, status, settings, appearance, version/About, diagnostics, clear cache,
and logout. It displays the selected account's real server/login/status; it must
not invent profile metadata missing from the native controller.

- [ ] Open each nested screen and return to the same account root. Keep the
  expanded external-account section and scroll position when appropriate.
- [ ] Confirm logout from the selected organization, remove only that session,
  and choose the appropriate remaining-session/login destination. Test cancel,
  expired credentials, another remaining account, and a response arriving after
  logout. Reference: [session removal](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/entities/workspace-auth/workspace-auth.lib.ts).
- [ ] Confirm Clear cache separately from logout. The reference
  [cold-start reset](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/shared/lib/local-reset.ts) clears local
  caches/storage while preserving saved organization logins and reloads the app.
  Define equivalent native ownership and failure behavior before implementing it;
  do not silently broaden this action to credential-vault or encryption-key erasure.

General settings acceptance:

- [ ] Chat click action: open General/default conversation or the latest topic,
  including the reference explanation of the avatar action.
- [ ] Notification sounds: default, subtle, digital, glass, pulse, or none;
  selection updates the actual notification policy. Sound selection is not proof
  that an OS notification is delivered.
- [ ] macOS-only dock-bounce switch affects native attention requests. Do not
  present a working dock-bounce action on Windows/Linux.
- [ ] Language changes all owned UI, plural rules, dates, and dialogs without
  altering message content. Accept Russian labels at their actual lengths.
- [ ] Idle timeout supports 6h, 12h, 24h, 3d, 7d, and never. Exercise expiry,
  activity, suspend/resume, and account-specific restoration with the session core.
- [ ] Open the full shortcut editor described below.

Appearance acceptance:

- [ ] Collapsible Theme, Sorting, Folder layout, and Density option groups preserve
  their selection states. Match the reference nested surfaces and row separators.
- [ ] Theme mode uses a three-column light/dark/system control with 24px-high
  buttons and mode-specific SVG glyph sizes. Palette selection uses a two-column
  grid of 32px-high rows with accent swatches. Preserve all four palette choices.
- [ ] System mode follows actual OS changes. Local light/dark toggling alone does
  not satisfy this state.
- [ ] Sorting supports last message, unread first, and alphabetical. Verify actual
  list order and live unread/message changes, including stable ties.
- [ ] Folder layout supports vertical/horizontal; density supports standard/compact.
  Verify changes in real folder/sidebar geometry and keyboard navigation.
- [ ] Restore preferences for the correct server/account/project scope after restart
  and after switching sessions. Handle invalid/unavailable storage without crossing
  ownership boundaries. The current native preferences persist appearance by
  opaque owner and sidebar width globally; full packaged restart, Electron
  migration and the other preference groups remain unaccepted.

### Shortcut dialogs

Reference: [shortcut settings](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/features/hotkey-settings/hotkey-settings.ui.tsx),
[shortcut policy](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/shared/lib/hotkeys/hotkeys.lib.ts),
[shortcut stories](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/features/hotkey-settings/hotkey-settings.stories.tsx).

- [ ] Search and group configurable commands; expose enabled state, current/default
  binding, fixed shortcuts, reset all, and a no-match state.
- [ ] The recording dialog receives focus directly. Record physical key positions,
  display their reference labels, and support save, disable, reset, and cancel.
- [ ] Reject reserved/invalid/conflicting combinations. A failed persistence write
  remains visible and must not report a successful change.
- [ ] Verify alternate keyboard layouts, macOS Command versus Control, Shift,
  AltGraph/IME, modal focus, text inputs, and fixed Escape/Tab/Enter/editing/F6 rules.

## Profiles, status, and avatar editing

References: [profile view](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/widgets/right-panel/right-panel-user-profile.ui.tsx),
[profile actions](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/widgets/right-panel/right-panel-user-profile-actions.ui.tsx),
[profile details](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/widgets/right-panel/right-panel-user-profile-details.ui.tsx),
[profile editor](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/features/user-profile/user-profile-editor.hook.ts).

- [ ] Resolve the exact UUID and distinguish self/other/deactivated users. Display
  real profile fields, custom status and measured presence with honest unavailable
  values. Missing values are not evidence for a default role, phone, or email.
- [ ] Match a 64px header avatar, 16px header gap, 20px/24px display name, 12px/16px
  status text, and reference action chips. Detail rows use 32px icon boxes, 24px
  glyphs/copy controls, a 12px row gap, 36px ordinary row height, 12px labels and
  14px values; multiline/editable rows expand.
- [ ] Self actions open Favorites, Edit, and Share. Other-user actions open/create
  the correct direct conversation, initiate its call workflow, and Share. Pending
  direct-chat creation prevents duplicate dispatch and navigation into another owner.
- [ ] Share copies a valid scoped profile link and shows the temporary copied
  glyph/state. Open shared links across signed-in/signed-out and multi-account
  states. Do not turn a failed clipboard write into success.
- [ ] Read-only detail fields include user ID, email, phone, job/manager/role,
  account type/status, time zone/local time, joined date, birthday and description
  only when supplied by the reference projection. Copy is unavailable for missing
  or multiline values where the reference does not offer it.

Own-profile editing uses the current IAM-backed facade, not the legacy profile
stub: [profile API](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/features/user-profile/user-profile.api.ts),
[editable fields/validation](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/features/user-profile/user-profile-editor.lib.ts).

- [ ] Load the current own profile before editing; preserve username as read-only.
  Editable fields are first name, last name, phone and description. Match trimming,
  empty/null behavior and actual patch semantics; do not add email/role editing.
- [ ] Validate name length/newline and phone length against the source constraints.
  Test empty names, Unicode, 128-character boundaries, and the 15-character phone
  boundary. Description remains multiline.
- [ ] Save only changed fields, gate duplicate submits, preserve draft on failure,
  and distinguish forbidden, invalid, unsupported, transient and stale results.
- [ ] If IAM save succeeded but Messenger projection refresh failed, show refresh
  pending; do not falsely claim the save failed or resend it automatically.
- [ ] Cancel restores the loaded profile/avatar baseline. Account/profile changes
  abort/invalidate editor requests and release temporary preview images.

Avatar dialog reference: [avatar actions](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/widgets/right-panel/right-panel-user-profile-edit-avatar-dialog.ui.tsx).

- [ ] Match the 323px-wide, 12px-radius dialog with 12px horizontal/20px vertical
  padding, 32px action rows, 8px row gaps and a 40px Cancel button.
- [ ] Offer Gallery and Remove current photo; removal is disabled without a photo.
  The desktop camera action is intentionally hidden and is not a required working
  camera button in this baseline.
- [ ] Validate file size/type, preview the staged upload/reset, and apply only through
  Save changes. Exercise cancel/reselection, invalid/empty/oversize files, save
  errors, permissions and expired session. The facade advertises a 25 MiB maximum;
  use its validation/accepted MIME contract rather than accepting arbitrary files.
- [ ] Synchronize successful avatar changes into profile/header/message projections
  and cache, without exposing another owner's late response.

Status dialog reference: [status controls](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/widgets/right-panel/right-panel-user-menu-status-dialog.ui.tsx),
[status mutation](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/entities/user/user-workspace-status-actions.lib.ts).

- [ ] Seed text/emoji and the manually chosen away state, rather than treating
  measured idle status as a manually enabled away toggle.
- [ ] Support presets, emoji picker, text input limit, away toggle, clear, save and
  cancel. Preserve the source 256-unit text truncation and emoji draft limits.
- [ ] Disable conflicting/dismiss actions during submit; show errors, keep drafts
  when saving fails, and update actual user/status projections after success.

## External accounts and provider management

References: [account list/integration](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/widgets/right-panel/right-panel-external-account.integration.tsx),
[connection dialog](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/features/connect-external-account/connect-external-account-dialog.ui.tsx),
[connection form](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/features/connect-external-account/connect-external-account-form.ui.tsx),
[lifecycle controller](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/features/connect-external-account/connect-external-account.hook.ts).

- [ ] Scope accounts to the selected runtime owner; accept loading, empty, cached,
  load-error and refreshed states. Match 40px provider rows, actual provider SVG,
  14px account label, 12px provider/lifecycle line and 24px actions control.
- [ ] The baseline provider choice is Zulip. Connect requires server URL, account
  identifier and a masked API-key field. Reject invalid URLs, missing fields and
  duplicate accounts. New providers require an explicit supported contract.
- [ ] Preserve create/checking/lifecycle/completed phases. An accepted account
  record is not equivalent to live-ready synchronization; show the actual lifecycle
  and safe errors, including auth-required/degraded/re-enter-credentials behavior.
- [ ] Edit preserves provider choice and existing settings; a blank replacement
  key does not erase stored credentials. Fetch current settings/ETag before a
  replacing update so unedited history/selection/provider options survive.
- [ ] Reconnect uses the same account UUID and current revision/ETag. Distinguish
  invalid credentials, forbidden external resource, conflict, unavailable provider,
  and request failure. Retry an uncertain create with its existing attempt UUID.
- [ ] Clear secret input on close/scope change and prevent secrets in logs,
  diagnostics, screenshots and ordinary persistence. Do not render a stored key
  back into an edit form.
- [ ] Delete uses a separate confirmation, pending gate and visible retryable error.
  Verify the selected account is removed from lists/cache; do not delete another
  account or unrelated messages as a UI shortcut. Reference:
  [delete dialog](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/features/connect-external-account/delete-external-account-dialog.ui.tsx).

External chat selection references: [configuration view](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/features/configure-external-chats/configure-external-chats-content.ui.tsx),
[configuration controller](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/features/configure-external-chats/configure-external-chats.hook.ts),
[capability helper](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/features/configure-external-chats/configure-external-chats.lib.ts).

- [ ] Reproduce explicit/all selection modes and history depth options new,
  7 days, 30 days, 90 days, all. Show unsaved/saving/saved/error/conflict states,
  load-current recovery, and warnings about selected/importing chats.
- [ ] Save account settings before allowing chat selection when the source blocks
  selection for dirty/pending settings. Automatic mode shows status markers rather
  than misleading manual checkboxes.
- [ ] Search the real catalog; show loading/preparing/empty/no-match/load-error
  states. Select all applies to visible eligible rows with an indeterminate state.
  Already selected or transition-pending rows are not selectable again.
- [ ] Start only pending selections, show ready/pending/failure counts, retry failed
  requests, and retain per-chat safe errors. Exercise partial success, close/reopen,
  account deletion and stale runtime responses during a batch.
- [ ] Preserve the reference capability/settings gates; missing capabilities or
  settings must not be interpreted as invented catalog data. Establish actual
  external delivery separately from selection/import acceptance.

Provider administrator references: [entry visibility](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/features/manage-external-provider/manage-external-provider-entry.ui.tsx),
[policy/health form](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/features/manage-external-provider/manage-external-provider-form.ui.tsx),
[provider controller](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/features/manage-external-provider/manage-external-provider.hook.ts).

- [ ] Show the administrator entry only after the actual access probe allows it;
  revoke/close it when permission changes. Do not infer administrative access from
  a display name or local UI role.
- [ ] Match the centered, scrolling dialog and fixed Save footer. Display real
  policy enabled/emergency state, health and updated time, and separate initial
  load failures from refresh failures with retained data.
- [ ] Edit limits for account count, selected chats/account and file MiB using the
  source numeric parsing/ranges; save only dirty valid state with ETag protection.
  On conflict, allow load current or retain draft without losing its ownership.
- [ ] Display custom-CA metadata honestly. The current desktop blocks policy save
  when an installed custom CA cannot safely be preserved by the update contract;
  do not silently drop or replace it.
- [ ] Show known health aggregates for accounts, chats, bridges, operations and
  metrics without fabricating zeros for absent values.
- [ ] Emergency suspend requires its confirmation state; resume uses its own pending
  state. Verify server state/read-back and refresh policy/health after each action.
  These operations belong to an authorized test environment during acceptance.

## Conversation creation, information, and folders

Creation references: [dialog/tabs](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/features/create-chat/create-chat-dialog.ui.tsx),
[creation controller](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/features/create-chat/create-chat-dialog.hook.ts),
[domain mutations](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/entities/messenger/messenger-create-chat-actions.lib.ts).

- [ ] Complete the actual Workspace caller's Start chat, Create channel, Create topic
  and Archive tabs with keyboard navigation and stable body height. The generic
  Browse tab is hidden by that caller. Ordinary Create channel is the current native
  slice; other tab slots remain explicitly unavailable. The reference has 240px
  direct-user lists, 160px channel-member selection and a fixed footer below the
  scrolling body.
- [ ] Direct chat searches actual selectable users, excludes unsuitable/self
  entries according to the source, prevents duplicate requests, and opens the
  resulting conversation UUID.
- [ ] Browse channels currently displays an explicit Workspace unsupported state.
  Its master/detail/filter shell is not evidence that public discovery or
  subscribe/unsubscribe works. Do not invent those operations for visual completeness.
- [ ] Create channel validates name, description, selected members, invite-only
  and announce flags. The separate announcement-only checkbox is explicitly
  unsupported/disabled. Preserve that distinction.
- [ ] Encrypted creation disables incompatible flags and performs the actual
  initial-key/bootstrap protocol. Preserve unresolved creation intent/retry state;
  never create another channel merely because the first outcome is uncertain.
  Keep this action unavailable in native UI until its crypto flow exists.
- [ ] Create topic requires a valid current channel and nonempty name, handles
  missing channels and invalidation, then selects the created UUID.
- [ ] Archived search uses real archived direct/channel records. Opening an
  archived conversation and unarchiving it are separate actions; show per-row
  pending/error state and do not turn a failed unarchive into disappearance.

Conversation information reference:
[channel/direct information](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/widgets/right-panel/right-panel-workspace-info.ui.tsx).

- [ ] Show actual title, description, channel type, participant/online counts,
  topic list and notification policy. Missing data stays unavailable.
- [ ] Add members uses real eligible users and a multi-selection dialog; remove
  affordances follow each member's actual capability. Test self-removal and roles
  through the supported contract, not a blanket local administrator assumption.
- [ ] Display notification changes pending/error states, update only the selected
  stream/topic, and retain distinct active/passive unread semantics.
- [ ] Topic list, close/reopen, rename, move and summary entry must preserve target
  UUIDs and permissions. Existing dialogs:
  [rename](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/features/mark-topic-resolved/rename-stream-topic-dialog.ui.tsx),
  [move](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/features/move-topic-to-stream/move-topic-to-stream-dialog.ui.tsx).

Folder references: [rail actions](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/widgets/folder-rail/folder-rail.ui.tsx),
[folder form](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/shared/ui/folder-form-modal/folder-form-modal.ui.tsx),
[context menu](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/widgets/folder-rail/folder-rail-context-menu.lib.ts),
[form stories](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/shared/ui/folder-form-modal/folder-form-modal.stories.tsx).

- [ ] Create/edit name and background color, preview the chosen color, and submit
  the actual API operation. Match the 520px maximum form width, 36px circular
  swatches, 10px swatch gaps, and 12%-alpha preview background.
- [ ] Trim empty names, disable unchanged edits and duplicate submit, focus name
  on open, support Enter, preserve failures, and close only after success.
- [ ] Delete a custom folder through confirmation; update selected-folder fallback
  and lists after server confirmation. Verify retained channels/messages.
- [ ] Rename/delete stay disabled for system folders. Layout and show/hide-system
  folders remain separate local-preference actions.
- [ ] Context actions assign/unassign chats and pin/unpin folder items through
  their actual owner-scoped mutations. Preserve colors, selection, unread badges,
  and scroll position in both vertical and horizontal layouts.

## Administrative, encrypted, and auxiliary screens

Topic-summary settings reference:
[permission-filtered dialog](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/features/topic-summary-settings/topic-summary-settings-dialog.ui.tsx).

- [ ] Render only authorized Topic, Gates and Endpoints tabs; hide the entry when
  none is authorized. Preserve keyboard tab navigation and different scroll/body
  heights. Reference maximum width is 70rem, header 5.5rem, tabs 3.25rem.
- [ ] Topic edits enablement, default/custom system prompt and default/off/minimal/
  low/medium/high reasoning. Preserve dirty/cancel/save/validation/forbidden/error
  states and the correct topic identity.
- [ ] Gates show global/project switches and their effective combination, permission
  note, load/refresh errors and independent save state. Do not equate one toggle
  with an effective system-wide result.
- [ ] Endpoints support list/create/edit/delete, masked write-only key entry,
  name/model/base URL, priority, maximum output tokens, enabled/vision/reasoning,
  temperature/top-p/presence/frequency settings, validation and confirmation.
  Reference: [endpoint view](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/features/topic-summary-settings/topic-summary-endpoints-section.ui.tsx).

Encrypted information reference:
[screen contract](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/features/encrypted-chat-info/encrypted-chat-info.types.ts),
[screen implementation](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/features/encrypted-chat-info/encrypted-chat-info.ui.tsx),
[E2EE protocol](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/docs/E2EE.md).

- [ ] Reproduce information, read-only settings, current-key and local-key screens.
  Show key identifiers/timestamps/current-or-historical state, not secret key bytes.
- [ ] Implement real missing-key requests, grant/reject/all-grant, rotation,
  historical local-key request/delete, and pending/failure/permission states.
  A placeholder encrypted message does not accept these flows.
- [ ] Keep key requests bound to request UUID, sender/device, key UUID and owner;
  do not apply another chat's action after navigation. Match the reference deletion
  behavior and verify the actual local-storage consequences for the selected key.

About/diagnostics references:
[About](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/widgets/right-panel/right-panel-about.ui.tsx),
[update controls](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/features/app-update/app-update-settings.ui.tsx),
[licenses](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/pages/licenses/licenses-page.ui.tsx),
[diagnostics](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/pages/logs/logs-page.ui.tsx).

- [ ] About uses the actual native build version, update status and license inventory.
  Accept checking/available/downloading/ready/error states and actual restart/install
  behavior on each OS; do not reuse the baseline version as the new build version.
- [ ] Licenses search/filter/reset uses the dependencies actually distributed with
  the native app and reused assets, with preserved notices.
- [ ] Diagnostics provides useful native transport/connection/cache/memory state,
  refresh, copy report/snapshot, and filtered logs. Keep missing metrics unavailable
  and redact credentials, message content and personal data before copy/export.
  Browser-specific performance fields need an explicit native counterpart or limit.

Additional message-related dialogs remain part of full parity: destination picking
and forwarding, reader lists, reaction details, time information, attachment/media
viewer and call/incoming-call windows. Their source entries are collected in
[Native desktop parity](NATIVE_DESKTOP_PARITY.md); do not treat completion of the
account drawer as completion of those independent flows.

## Delivery sequence and closure gate

1. Establish secure session storage and realtime/runtime ownership in the core.
   Then implement account root/settings/appearance as a complete persistence-backed
   flow. Keep credentials out of ordinary settings and appearance objects.
2. Implement profile/status/avatar and folder flows with controller mutations,
   read-back, error recovery and per-owner invalidation before exposing working UI.
3. Implement creation/membership/notification actions, then external-account
   lifecycle/catalog/provider administration with end-to-end integration proof.
4. Complete encryption, summaries/endpoints, update/diagnostic and other modal
   workflows with their separate permission and platform requirements.

For every screen and action, record actual implementation, automated regression,
native interaction, visual comparison and live integration results separately.
Run the same complete flow on Windows, macOS and Linux; include matching theme,
locale, font, viewport/DPI, focus, hover, selection, loading, empty, forbidden,
invalid, offline, conflict, pending and successful states. A representative screenshot
must not replace missing interaction checks or another operating system's run.

Provide updated reference/native screenshots during progress with the tested state
and remaining differences. Use deterministic fixtures, crop only documented OS
decorations, and inspect overlays/differences as specified in the parity document.
Do not claim near-pixel acceptance from the existing unmatched-content pair.

Before closing a screen, verify server-confirmed outcomes for remote actions,
keyboard/assistive access,
cancel and retry behavior, preservation of newer edits, stale-response isolation,
and orderly disposal of its subscriptions, temporary image URLs and resources.
Any unavailable native platform, external endpoint or visual state remains
`BLOCKED` or `NOT RUN`. This document is a checklist; it contains no completed
platform acceptance results.
