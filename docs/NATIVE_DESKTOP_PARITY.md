# Native desktop parity

The latest fully accepted baseline is the imported V34 snapshot. Current ordinary-channel
response changes are in progress; see [current development](CURRENT_DEVELOPMENT.md)
for the checks and unresolved observations on this branch.

The native desktop client is **IN PROGRESS**. It targets Windows, macOS, and Linux.
The required result is an almost pixel-perfect reproduction of the current desktop
design, complete functional parity, and full testing of the new application on
all three operating systems. These are independent acceptance requirements.
The current implementation and acceptance order is **Linux first**: finish the
current Linux client as a complete copy before evaluating other distributions or
resuming Windows/macOS acceptance. A bounded application cache is mandatory;
measure repeat-navigation API requests and storage/CPU/memory/network costs, not
just the time until a cached list appears.
Its initial work covers authentication, channel/topic navigation, and message
reading and sending. It does not yet establish full parity with the existing
desktop application. A successful build or a visible message list is not evidence
that the scenarios below have passed acceptance.

## Baseline and scope

The implementation baseline is the `workspace_ui` desktop source at revision
`95495b10b828f853c9e770b47216073eb973fa95`, version **0.9.11**. The existing desktop
application combines the React application in `packages/web` with the Electron
shell in `packages/electron`. Source paths in this document describe that baseline;
later changes to those files must be assessed when updating parity requirements.

A read-only upstream check on 2026-11-07 found one newer desktop change,
`61dacef78dab2f5d0be76f283bebf49fe29b84af`: canonical topic routing after moves or
merges, plus message placement updates in loaded and cached conversations.
The matched creation-dialog references below still use the pinned baseline.
This later behavior must enter the native routing/cache acceptance before full
parity can be claimed; no current creation-dialog result establishes it.
The known indexed-body retention case after a move is now covered separately in
[current development](CURRENT_DEVELOPMENT.md). This does not establish all of the
newer global topic catalog, browser-location and routing behavior.

The inventory comes from routes, callers, implementation, and tests. Historical
feature lists in `USE_CASES.md` do not establish that a feature is available in the
current Workspace integration. A desktop control that displays an explicit
unsupported state belongs to the parity surface, but does not demonstrate backend
support for the corresponding operation.

The scope includes the live Messenger application, account and project isolation,
local data, and operating-system integration. Mail and Calendar remain in scope
with the behavior the desktop actually exposes: configured embeds or an unavailable
state. Implementing complete new mail/calendar clients is a separate capability
from reproducing these desktop entrypoints.

## Native architecture decision

Use **C++17 and Qt 6 Quick**, with a single native QML `ApplicationWindow`, for the
product presentation on all three operating systems. Preserve the C++ transport,
session, and domain layer. The earlier Widgets test window is a temporary technical
harness that is being replaced; it is not the final product design or evidence of
visual parity. The Messenger interface must use the native Qt rendering stack;
embedding the React application in a WebView does not complete this migration.
Use Qt networking for the Workspace REST and realtime transports and CMake for
build configuration. Keep the initial structure small: a transport boundary for
Workspace/IAM requests, explicit session/domain data, and native views consuming
that data. Add further layers only when a concrete workflow requires them.

Preserve the Workspace UUIDs, API contracts, and error behavior. Access external
providers through Workspace, without introducing direct provider fallbacks. Reject
responses from a superseded account, project, or session generation before they can
update a view, persistent data, or unread counts. The reference ownership model is
[runtime ownership](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/entities/workspace-runtime/workspace-runtime.lib.ts)
and [async safety](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/docs/ORG_SCOPED_ASYNC_SAFETY.md).

Remembered native sessions use the [session vault](../packages/native/src/session-vault.cpp)
and QtKeychain with plaintext fallback explicitly disabled for reads, writes and
deletes. The private settings file holds account metadata only. Credential keys
bind a canonical HTTPS origin, project UUID and user UUID; no passwords are saved.
Persistent catalogs and history use the bounded owner-scoped SQLite cache.
Device and generation keys use the separate encrypted vault described in
[native crypto](NATIVE_DESKTOP_CRYPTO.md). Durable drafts, scroll positions and
the outgoing-message queue remain incomplete.
Cached data and optimistic
state must never substitute for server confirmation. Import of existing desktop
data and actual operating-system secure-store acceptance remain open.

Calls require an explicit integration decision. The existing Jitsi conference UI
is an embed with Electron screen-sharing support; Qt Widgets alone supplies neither
Jitsi conference compatibility nor screen capture. Any limited embedded conference
surface must be documented, permission-scoped, and accepted independently of the
native Messenger UI.

## Locked visual requirements

The current desktop application is the design reference. Preserve its hierarchy,
geometry, spacing, typography, colors, icons, clipping, visual states, and interaction
feedback. A native platform's default widget appearance does not replace this
reference. Use the source below together with matched desktop screenshots;
source constants alone do not establish the final rendered geometry.

| Visual contract | Source-backed specification |
| --- | --- |
| Theme tokens | Use every semantic token in [token schema](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/shared/lib/themes/tokens.ts) and the exact light/dark values in the [palette directory](../packages/web/src/shared/lib/themes/). The [registry](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/shared/lib/themes/registry.ts) contains Orange Warm, Blue Cold, Blue Mist and Emerald Chat; Blue Cold is the default palette. System mode resolves against the OS. Keep distinct page/chrome/card/input/peer/own/selected/background, text, icon, border, notification and indicator tokens. |
| Typography | [Global CSS](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/app/app.styles.css) uses `system-ui, -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, "Helvetica Neue", Arial, sans-serif`, without bundled font files. Match the reference's resolved font on each OS, not a Figma comment naming Inter. Preserve actual size, weight, baseline and line height: common message/topic text is 14px; topic titles use 20px line height/medium weight; composer input is 16px; metadata commonly uses 12px or 11px. |
| Grid and radii | [Tailwind configuration](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/tailwind.config.ts) uses 4px-based spacing with explicit half-steps and exceptions. Its radius overrides are `lg=6px`, `xl=10px`, `2xl=12px`, `bubble=18px`; do not substitute Tailwind's default radii or round explicit 2px/3px/38px values to the grid. |
| Desktop hierarchy | [App shell](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/widgets/layout/layout-app-shell.ui.tsx) places TopBar above [main workspace](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/widgets/layout/layout-main-workspace.ui.tsx). The body contains optional folder rail/sidebar, flexible routed content, optional right drawer, with 4px gaps. Overlays include dialogs, notifications, media viewer and call windows. |
| Sidebar and rail | Sidebar defaults to 299px and resizes using viewport-relative bounds in [sidebar sizing](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/widgets/layout/layout-sidebar-width.lib.ts). Vertical [folder rail](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/widgets/folder-rail/folder-rail-vertical-view.ui.tsx) is 72px wide; horizontal mode is 44px high. Normal rows have 10px padding/12px gap and 40px avatars; compact rows have 8px horizontal/6px vertical padding, 8px gap and 32px avatars. [Topic rows](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/widgets/sidebar/sidebar-chat-row-layout.lib.ts) use a 38px normal left inset, 3px color strip, 12px gap, and 2px topic-to-topic spacing. |
| TopBar | [TopBar](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/widgets/top-bar/top-bar.ui.tsx) uses 8px toolbar padding and 4px bottom gap. Section buttons are 48px square with 12px radius, 32px glyphs and 12px inter-button gaps; the section group is centered independently of left/right slot widths. macOS adds a 28px traffic-light clearance strip. Match the actual rendered header; a named spacing token is not proof that the header has that fixed height. |
| Chat header and drawer | [Chat header shell](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/widgets/chat-view/chat-header-shell.ui.tsx) uses 20px horizontal/8px ordinary vertical padding, 6px radius, and 20px action glyphs with 8px button padding. Encrypted header content has its own spacing. [Right drawer](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/widgets/right-panel/right-drawer.ui.tsx) is 315px wide with 6px radius; content/header padding and scroll ownership must match. |
| Messages | [Bubble presentation](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/widgets/workspace-message-list/workspace-message-bubble-presentation.ui.tsx) sets maximum width to 88%, padding to 12px horizontal/8px vertical, 14px text, 18px corners and a small shadow. Last-in-group own/peer tails use a 6px lower corner; grouped sides use 10px. [Message list](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/widgets/workspace-message-list/workspace-message-list.ui.tsx) uses 12px horizontal/16px vertical padding, 8px group gaps and 4px within author groups. Preserve author/avatar columns, inline metadata, reactions, quotes, selection and anchor highlights. Metadata uses 12px text/16px leading; a final root paragraph reserves its measured width plus 8px. Lists, quotes and other block tails, or messages with reactions, retain the separate footer. See the [DOM anchor](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/widgets/workspace-message-list/workspace-message-bubble-inline-meta.hook.ts) and [reserve CSS](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/widgets/workspace-message-list/workspace-message-bubble-meta.css). |
| Composer | [Composer](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/widgets/message-composer/message-composer.ui.tsx) uses a 10px outer/input/send radius, 8px container padding, a 40px send button and 12px send/input gap. Input minimum height is 40px; automatic text area maximum is 128px. [Toolbar geometry](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/widgets/message-composer/message-composer-styles.lib.ts) defines 32px hit areas, 24px ordinary glyphs, precise non-square glyph sizes, and 52px send clearance. Match compact/expanded/manual-resize/reply/edit/attachment states separately. |
| Assets | Reuse the actual [SVG icons](../packages/web/src/shared/assets/icons/) and media assets with original view boxes, aspect ratios, optical bounds and per-state colors. Many use `currentColor`; explicitly bind it to the appropriate token in the Qt renderer. The repository [license](../LICENSE) and [package declaration](../packages/web/package.json) identify Apache-2.0; retain applicable license/attribution information when packaging assets and dependencies. The inspected SVGs have no individual attribution headers; this is not an independent provenance audit. |

For example, Blue Cold dark uses page `#141517`, chrome `#222328`, card/peer
`#282a32`, active card `#2c3747`, own bubble `#252942`, accent `#7087ff`, and
border `#33384d`. Blue Cold light uses page `#e4ecf3`, white chrome/peer bubbles,
card `#eef5fb`, active card `#cde6ff`, own bubble `#aed7ff`, and border `#d8e4ef`.
These examples do not replace the complete palette source or the other palettes.

### Native rendering adaptation

The chosen Qt Quick [scene graph](https://doc.qt.io/qt-6/qtquick-visualcanvas-scenegraph.html)
uses one scene for TopBar, body layout, virtualized sidebar/messages, composer and
overlays, backed by the C++ session/domain/transport layer. Custom-painted Widgets
were considered, but platform-default styles would require extensive replacement
to reproduce the same control geometry and nested visual states. The single Quick
window avoids a separate embedded Quick surface per pane:
[QQuickWidget](https://doc.qt.io/qt-6/qquickwidget.html) adds an offscreen pass and
disables the threaded render loop.

The initial [QML scene](../packages/native/qml/Main.qml) binds to actual controller
models, renders account labels and other untrusted metadata as plain text, exposes
supported message actions, and marks unavailable controls explicitly. Message
bodies use the [native Markdown renderer](../packages/native/src/native-markdown.cpp),
which generates restricted Qt rich text instead of accepting raw message HTML.
Its [palette object](../packages/native/qml/NativeTheme.qml) contains the four
desktop palettes in both modes. Initial/name-based avatar fallbacks do not
reproduce profile images. The organization button reuses the source fallback SVG;
custom organization logos still require real metadata and an accepted load policy.
[Native preferences](../packages/native/src/native-preferences.cpp) persist the
four palettes and light/dark/system mode per opaque account-owner hash, with
anonymous preferences stored separately. Sidebar preferred width is global. System
mode follows Qt style hints, and invalid stored values fall back safely. This does
not establish complete preference parity or migration of Electron local storage.

A local component reference mounts the pinned desktop presentation code with
synthetic messages, independent of live Workspace data. Closed-message and default
picker scenes are compared at 1440×900, display scale 1, equal locale/palette and
light/dark state. This establishes a component baseline only. Full-shell,
production-service, Electron and cross-platform acceptance remain separate.
Measured body font resolution on the current Linux reference is Ubuntu Sans in
both renderers; do not change the family to conceal a width or wrapping defect.
The ordinary header/history flow has no extra gap; history supplies its own 16px
top inset. Peer fallback avatars follow the generated source primary text color,
18px semibold text and 48px frame, with visible light/dark coverage. Source sticky
avatar rails and actual avatar images still require acceptance.

### Initial session, rendering and realtime boundaries

- [Session](../packages/native/src/session.cpp) remembers a refresh credential only
  after the selected project and user pass introspection, and saves subsequent
  verified rotations. Saved-account labels expose metadata only. Continue performs
  refresh and checks the expected user/project before showing the authenticated
  shell. Remove and explicit sign-out forget the selected identity; internal login
  resets and offline restoration failures preserve unrelated saved entries.
  A secure-store warning does not cancel a verified session or an in-flight
  realtime snapshot. The profile trigger exposes only verified public IAM identity
  fields, uses plain text/elision and a 36px initial avatar, and clears on new
  sign-in or sign-out. Later owned Users projections add uploaded profile images,
  static normalized presence, own status labels and display email; profile editing,
  manual status/reporting and complete geometry remain open.
  Idle timeout, background-account operation, migration and
  actual OS keychain behavior remain outside this initial implementation.
- The Markdown boundary includes native bold/italic, code, lists and other
  supported Qt Markdown formatting. Raw HTML remains text, unsafe link targets lose
  link behavior, and resource loading is disabled. Credential-free HTTP/HTTPS links
  can open externally. Secret-fragment labels/envelopes stay masked in visual and
  accessible message text. Workspace file/image/video references can open the
  authenticated save flow; profile, conversation and quote navigation and media
  previews still show an explicit unavailable notice. This does not establish
  complete Workspace rendering parity.
- Native message ownership follows the [desktop bubble](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/widgets/workspace-message-list/workspace-message-bubble-presentation.ui.tsx):
  the author is the current verified viewer **or** the server own flag is true.
  [Controller regressions](../packages/native/tests/native-controller-test.cpp)
  cover all four author/flag combinations and the corresponding edit/delete
  guards. This view rule does not replace the stricter server facts required to
  confirm a locally sent message. Matched visual fixtures use a distinct peer
  author and do not substitute for these ownership checks.
- [File transport](../packages/native/src/file-client.cpp) supports authenticated
  multipart upload, validated metadata and atomic download with size/checksum
  checks. The initial native dialogs choose files for upload and save a selected
  file reference; completion notices are plain text. Picker acceptance is fenced
  by owner and conversation, and save acceptance by owner. Numeric progress,
  transfer cancellation/retry controls, attachment editing/previews and actual OS
  picker/portal acceptance remain open. The visible automated flow uses Qt's
  fallback dialog with real local HTTP transfers and synthetic files.
- [Realtime transport](../packages/native/src/realtime-client.cpp) uses an epoch
  baseline, a controller snapshot barrier, paginated REST catch-up and WebSocket
  wakeups. Event acknowledgement advances an in-memory owner-scoped cursor only
  after controller application; generation changes, cursor expiry and reconnect
  have explicit recovery paths. [Controller reducers](../packages/native/src/native-controller.cpp)
  currently cover selected message, topic, stream, read and user-name updates.
  Durable cursors/outbox, background accounts, full unread/presence behavior
  and live server acceptance remain open.
- The [response cache](../packages/native/src/native-cache.cpp) stores typed
  catalog and history pages in owner-isolated SQLite partitions. It enforces a
  configurable global physical budget, default **10 GB (10,000,000,000 bytes)**,
  including database/WAL/shared-memory files. Settings allow 1–1000 GB without
  preallocation or the former 512-page/32/64 MiB ceilings. Entries remain bounded
  by the 8 MiB response limit; SQLite page cache and warm-fresh bookkeeping stay
  small. Global LRU deletion/checkpoint/vacuum reclaim files after reduction.
  Large reduction/cold-open maintenance runs on the serial storage worker and
  has separate latency/temporary-peak acceptance from ordinary cache operations.
  Bounded LRU compaction prevents a new whole-cache freelist; an existing large
  freelist retains the cancellable incremental fallback. The full synthetic
  10 GB worker workload completed reduction to 1 GB in 137.177 s, preserved the
  newest page through cancellation/reopen and removed its test data. See the
  [measurement scope and remaining gaps](NATIVE_DESKTOP_CACHE_BENCHMARK.md).
  Schema v2
  indexes message/page membership, retains raw stale confirmed-read flags and
  avoids whole-cache scans on ordinary hits/writes. Its lease rejects late
  writes after an owner or cache-service change. Freshness is never restored just
  from an on-disk timestamp; cold data requires revalidation. Storage regressions
  cover actual blobs, expiry, owner isolation, corruption and encoded payloads.
  Warm visible navigation reuses verified pages: four topic selections make two
  history GETs, and two stream selections make one topic-list GET in the synthetic
  regression. A two-process visible test hydrates saved rows before delayed GETs,
  retains them during failure/retry, replaces updated/deleted history and prevents
  another verified identity from seeing the previous rows. A source-style loading
  chip and compact retry state expose this boundary; mutations/read intents wait
  for authoritative revalidation. In-progress edit text survives revalidation with
  Save disabled. Full offline authentication, live reconnect and broad resource
  cost acceptance remain open. This response cache does not establish a durable
  realtime projection or outbox.
- The [visible-read scene](../packages/native/qml/Main.qml) batches for 250 ms,
  then resolves the bottommost loaded bubble intersecting the current viewport.
  It requires an active visible nonminimized window, a selected ready conversation
  and no blocking menu/dialog/picker. Inactive or hidden history does not itself
  create a read intent. The controller confirms the exact marker before updating
  loaded flags and fetching active/passive unread counters. Initial unread
  positioning, durable scroll anchors, topic/stream bulk read and notifications
  remain incomplete. A real two-window synthetic regression covers focus,
  viewport geometry, minimization, hiding and restoration.
- The message scene reconciles rows by UUID and preserves the first visible bubble
  and offset for incoming appends while reading history and for older-page
  prepends. Incoming tail following uses the source 80px bottom threshold. The
  exact verified local-send signal pins the newly inserted own row; an own row
  from realtime does not create a local send intent. This does not establish
  durable conversation position, initial unread opening or outbox parity.
- Reaction chips use native Unicode and confirmed own-state metadata. The
  320×360 [picker](../packages/native/qml/NativeReactionPicker.qml) virtualizes the
  source category/order metadata and 1,895 eligible neutral bases, with search,
  six skin tones, source variation mapping and a fourteen-entry suggested history.
  All 3,979 wire identities remain in the core. The bounded
  [quick capsule](../packages/native/qml/NativeQuickReactions.qml) combines six
  base choices with at most five popular reactions from seven UTC days. Picker
  clicks and confirmed new quick additions record their separate owner-scoped
  histories at their respective source lifecycle points. Category/skin state,
  mouse and basic keyboard paths have visible synthetic coverage.
  The parent message menu stays open behind the picker. On the current Linux
  reference, picker labels resolve to Liberation Sans and emoji to Noto Color
  Emoji; the native font rule is calibrated by painted ink inside fixed 40px
  cells, not by nominal font-size equality. Other platforms remain unmeasured.
  Unknown provider labels stay plain text; removal resolves the actual own
  identity, and uncertain writes expose confirmation-only checks. Complete
  participant lists show overlapping initial avatars with verified-directory
  labels or a UUID-prefix fallback; partial lists retain counts. Names/tooltips
  cannot interpret markup. Encrypted reactions never disclose body content.
  [Presentation geometry](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/widgets/workspace-message-list/workspace-message-bubble-presentation.ui.tsx)
  and [picker geometry](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/widgets/workspace-message-list/workspace-message-bubble-menu.ui.tsx)
  remain the source references. Actual avatar images and full keyboard,
  accessibility and matched visual acceptance remain open. Profile and shared
  button focus rings use keyboard-visible focus, covered by a mouse/keyboard
  regression.
- The message menu uses source SVG icons, semantic palette colors and measured
  quick-bar/action-row geometry. Reply precedes Copy text; only verified own
  readable messages show Edit/Delete. Locked bodies do not expose unsupported
  body actions. Home/End and arrow/Return paths have visible coverage, and
  owner/route changes still close the menu and picker. The native Star extension
  was removed because the full pinned source caller has no such context action;
  the core method remains separately tested. Source Add reply, forwarding, selection,
  contextual image/link actions and forward-snapshot edit restrictions remain
  incomplete; this is not the full source caller permission matrix.
- A resolved selected topic hides and disables the composer, retains the draft
  through verified realtime reopening and blocks sends/uploads in the core. Its
  source-style prompt opens a one-item menu for the verified reopen action.
  Mouse/keyboard regressions confirm a single POST followed by canonical topic
  GET; pending or uncertain confirmation cannot trigger another toggle. This
  does not establish the remaining topic-management flows.

The [vault](../packages/native/tests/session-vault-test.cpp),
[session](../packages/native/tests/session-test.cpp),
[realtime](../packages/native/tests/realtime-client-test.cpp),
[Markdown](../packages/native/tests/native-markdown-test.cpp) and
[visible Quick-window](../packages/native/tests/native-window-test.cpp) tests use
synthetic fixtures and injected credential backends where appropriate. They do
not establish Windows/macOS acceptance, real secure-store access, live restoration,
cross-client event delivery or matched visual acceptance on any OS.
The [preferences tests](../packages/native/tests/native-preferences-test.cpp)
recreate the settings object to verify restoration and exercise Qt system-appearance
signals. Actual OS appearance switching, native file pickers, packaged restart and
Electron settings migration require separate acceptance.

Refine the visual layer using a representative sidebar, message, composer and
overlay comparison. Rendering technology and a clean QML lint result do not
establish fidelity. Preserve C++ domain ownership and tests when adapting the
presentation.

### Visual regression acceptance

Maintain a reference/native pair for **each operating system**. Match the desktop
revision, logical content viewport, actual resolved font, palette, light/dark/system
resolution, locale, text size, display scale/DPI, device pixel ratio, sidebar width,
folder layout, density, route, scroll position, fixture content and interaction
state. Map CSS logical pixels to Qt device-independent coordinates; do not multiply
the same geometry by scale twice. Compare the content area separately from native
window decorations and retain platform-specific decoration checks.

The visual matrix covers all four palettes in light/dark, both locales, standard
and compact density, vertical/horizontal folders, default/minimum/resized sidebar,
right drawer open/closed and representative supported window sizes. Include the
actual captured reference viewport and controlled 1440×900 content fixtures where
supported, plus smaller/larger supported desktop viewports. The reference
[Electron window](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/electron/src/main.ts) defaults to 1440×900 and has a
700×600 minimum; these window bounds must not be confused with its content bounds.
Test 100%, fractional and 200% display scale where supported by the platform.
System mode requires a live OS theme-change check. A screenshot from a different
OS/font or a scaled image is not an equivalent baseline.

Capture normal, hover, pressed, focus, selected, disabled, empty, loading, error,
offline/reconnecting, unread/muted, reply/edit, attachment progress/failure,
encrypted/missing-key, incoming/active-call, menus/dialogs and media-viewer states.
Use deterministic synthetic content/time; freeze or capture animation at a defined
phase. Pair screenshots with widget interaction/accessibility checks so painted
content cannot pass while controls are unusable.

Produce overlays and pixel/region differences plus a human review of geometry,
text wrapping/baselines, icon bounds, color, clipping, radii, shadows and state
visibility. Establish explicit tolerances from each OS's font rasterization;
exclude only documented platform decoration or rasterization differences, never
whole controls or broken layout. Do not accept a generic similarity score as proof
that missing screens, actions or states match. Visual acceptance on Windows,
macOS and Linux is currently **NOT RUN**.

## Scenario inventory and implementation status

`IN PROGRESS` identifies the initial implementation boundary, not a completed or
accepted scenario. `NOT IMPLEMENTED` means the native parity requirement remains
open. `NOT RUN` in the acceptance plan means no platform acceptance is established.

| Scenario group | Existing desktop behavior and source | Native status |
| --- | --- | --- |
| Sign in and session lifecycle | Organization/server selection, credentials, OTP, project discovery/selection, refresh, logout, idle timeout. [Login](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/pages/login/login-page.ui.tsx), [IAM transport](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/shared/api/workspace-iam-auth.ts), [sessions](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/entities/workspace-auth/workspace-auth.model.ts). | IN PROGRESS: public Linux0.1.5 passed the primary cassi credential/OTP form and exact project selection. Refresh and secure remembered-session foundation have local coverage; complete lifecycle and real OS persistence/restoration remain unaccepted. See [primary-account checks](provenance/linux-primary-0.1.5-checks.json). |
| Multiple accounts and projects | Remembered sessions, account switching, organization/project-scoped routes, background account projection and unread indicators. [Session switcher](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/widgets/top-bar/top-bar-workspace-session-switcher.ui.tsx), [runtime ownership](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/entities/workspace-runtime/workspace-runtime.lib.ts), [background projection](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/entities/messenger/messenger-background-projection.model.ts). | IN PROGRESS for saved-account Continue/Remove on the sign-in screen; authenticated account switching, background projection and unread indicators remain open. |
| Catalog and basic navigation | Channels, direct chats, topics, channel/topic routes and message UUID routes. [Routes](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/app/app-route-definitions.tsx), [catalog model](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/entities/messenger/messenger.model.ts), [sidebar](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/widgets/sidebar/sidebar-workspace.ui.tsx). | IN PROGRESS; channel/topic navigation is the initial boundary. |
| Create and manage conversations | Start direct chat, discover/create channels, create topics, archived channels, privacy/encryption options, rename/archive/unarchive channels, close/reopen/rename/move topics. [Creation](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/features/create-chat/create-chat-dialog.hook.ts), [domain actions](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/entities/messenger/messenger-sidebar-actions.lib.ts), [move topic](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/features/move-topic-to-stream/move-topic-to-stream.api.ts). | IN PROGRESS; global New chat starts ordinary Direct chats and opens verified existing active encrypted native pairs, with valid or null current keys. Existing encrypted opening has focused Main/HTTP and same-Direct Source grant/read/send coverage; missing-key Send/Attach remain locked. Whole Window285/E2EE core274/UI141, public package and anonymous installed upgrade checks passed. Live/pixel/credential acceptance remains open; see [0.1.5 candidate checks](provenance/linux-0.1.5-candidate-checks.json). New chat also exposes ordinary unencrypted channel creation with selected initial members. Both use canonical confirmation, explicit recovery and aggregate navigation. Global and channel-context topic metadata creation share exact-intent recovery and canonical confirmation; ordinary/direct/encrypted/provider stream metadata have typed local HTTP/UI coverage. Topic close/reopen remain available. Archive, encrypted content/bootstrap and remaining management flows are open. The separate intermittent V17 channel-context menu acceptance failure remains unexplained. |
| Membership and permissions | Member selection, add/remove, roles and operation-specific capability checks. [Member actions](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/entities/messenger/messenger-stream-member-actions.lib.ts), [channel information](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/widgets/right-panel/right-panel-workspace-info.ui.tsx). | IN PROGRESS for initial members of a newly created ordinary channel and explicit same-stream retry. General membership editing, removal and role management remain open. |
| Sidebar and folders | System/custom folders, colors, folder assignments, pinning, sort/density preferences, previews, topic collapse/show-more and unread badges. [Folder rail](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/widgets/folder-rail), [sidebar](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/widgets/sidebar), [folder management](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/features/manage-folders). | NOT IMPLEMENTED |
| Read and send messages | Load topic and aggregate stream history; the ordinary stream view offers topic selection before composing. Send to the selected topic and preserve identifiers and server errors. The shared default-topic resolver does not establish a visible ordinary stream composer. [Chat workflow](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/pages/chat/chat-page-workspace.ui.tsx), [message actions](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/entities/messenger/messenger-message-actions.lib.ts). | IN PROGRESS: topic reading/sending and the mixed-topic channel feed with first-group labels, transition dividers and topic-choice rail are implemented. Ordinary aggregate sends/uploads/replies stay blocked. Public Linux0.1.5 passed real primary-account selected-topic history, one physical native Send with exact canonical UUID readback and observation of a new incoming message without manual reload. Full history, service-message, initial unread and matched visual acceptance remain open. See [primary-account checks](provenance/linux-primary-0.1.5-checks.json). |
| History and reading position | Initial unread position, older/newer pagination, cache restoration, scroll anchoring, return-to-conversation memory, targeted message navigation and deleted-pointer repair. [Conversation opening](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/features/workspace-conversation-opening), [anchor navigation](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/features/workspace-message-anchor-navigation), [message store](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/entities/message/message.model.ts). | IN PROGRESS for loaded-history pagination and visible UUID/offset preservation across append/prepend; initial unread opening, durable positions, targeted navigation and deleted-pointer repair remain open. |
| Message actions | Edit/delete, reply/quotes, multi-selection, forward, pin and reactions with own state and reader/user details. [Chat workflow](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/pages/chat/chat-page-workspace.ui.tsx), [forwarding](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/features/workspace-forward-message), [reaction actions](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/entities/messenger/messenger-message-reactions-actions.lib.ts). | IN PROGRESS for copy/quote/edit/delete controls and verified reaction chips, source categories/skins, bounded quick choices, separate recent histories and participant initial labels; actual avatar resources, complete behavior and remaining actions are not accepted. |
| Compose and drafts | Markdown formatting/preview, mentions and silent mentions, emojis/stickers, edit-last-message, resizable input, time/call-link insertion, reply state and persisted drafts. [Composer](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/widgets/message-composer), [draft lifecycle](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/entities/composer-draft), [draft API](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/shared/api/messenger-drafts.api.ts). | IN PROGRESS for ordinary/encrypted text and local drafts plus Unicode emoji search/selection in the actual composer. Repeated choices append at the end and restore focus; source-peer send and route/API lifetime checks pass. The 0.1.6 candidate adds four Source-default formatting shortcuts with selection/caret/scroll semantics, focused actual Main and independent encrypted-send checks plus representative physical Ctrl+B in EN/RU. Full Window293/encrypted UI142/locale14, local package/runtime and checkpoint CI passed; public delivery and the installed update remain pending. Toolbar, shortcut remapping, stickers, remaining formatting/mentions, persistent draft API and full composer presentation remain open. See [formatting checks](provenance/composer-formatting-checks.json) and [composer emoji checks](provenance/composer-emoji-checks.json). |
| Content and media | Sanitized Markdown, Workspace user/file/time/secret URNs, quotes/forward snapshots, link previews, image/file previews, gallery/zoom/thumbnails. [Rendering](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/shared/lib/workspace-message-render), [message body](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/entities/messenger/messenger-workspace-message-body.ui.tsx), [media viewer](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/features/media-viewer). | IN PROGRESS for restricted native Markdown and typed-reference parsing; full URN actions, media and rendering compatibility remain open. |
| Uploads and downloads | Upload-before-send, file validation, progress, cancellation/retry/removal, inline images and editing existing attachments; authenticated account-scoped downloads with cancel/open/reveal/dismiss. [Composer attachments](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/features/workspace-composer-attachments), [file API](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/shared/api/messenger-files.api.ts), [downloads](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/electron/src/workspace-downloads.ts). | IN PROGRESS for authenticated file transport, basic file-picking/save dialogs and completion notices; full attachment UX and actual OS picker/portal acceptance remain open. |
| Unread, notifications and presence | Active/passive unread counts, read boundaries, topic/stream/read-up-to actions, notification modes and sounds, deduplication/click routing, typing and presence. [Read actions](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/entities/messenger/messenger-read-actions.lib.ts), [visible reads](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/features/workspace-message-read/workspace-visible-message-read.hook.ts), [notifications](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/widgets/layout/layout-workspace-notifications.hook.ts), [typing](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/features/typing-indicator), [presence reporter](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/entities/user/user-workspace-presence-reporter.lib.ts). | IN PROGRESS for active visible viewport read-up-to with server confirmation and counter refresh; bulk read, notification and presence workflows remain open. |
| Inbox, feed and activity | Grouped unread Inbox, feed, starred messages, mentions with all/unread filters, drafts and Favorites through self-chat. [Inbox](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/pages/inbox/inbox-page.ui.tsx), [feed](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/pages/feed/feed-page.ui.tsx), [activity](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/pages/activity/activity-page.ui.tsx), [Favorites](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/pages/chat/chat-page.ui.tsx). | NOT IMPLEMENTED |
| Users, profiles and search | Search users, initiate direct chat, view/share/edit profile and avatar, custom status text/emoji. The global search modal filters users; it does not establish full-text message search. [Search](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/widgets/search-modal/search-modal.ui.tsx), [profiles](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/features/user-profile), [profile views](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/widgets/right-panel/right-panel-user-profile.ui.tsx). | IN PROGRESS for canonical Users names/display email, uploaded avatars and static presence/status labels in the actual Main, plus searchable ordinary active direct-chat initiation. The three-line profile geometry has matched local Source measurements and full Window237 coverage. Own status editing, full profile/right-drawer/global search, presence reporting, live account and full visual/platform acceptance remain open. |
| External accounts and providers | Connect/edit/reconnect/delete Zulip accounts, select bridged chats, show provider identity and delivery state; administrator policy, limits, health and suspend/resume controls. [Account connection](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/features/connect-external-account), [chat configuration](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/features/configure-external-chats), [provider management](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/features/manage-external-provider). | NOT IMPLEMENTED |
| Encrypted chats and secret fragments | Device identity and local keys, bootstrap, key announcement/request/grant/reject/rotation, encrypted payloads, secret fragments and key-information screens. [Crypto implementation](../packages/web/src/entities/messenger/chat-real-crypto-provider.lib.ts), [E2EE controller](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/entities/messenger/chat-e2ee-controller.lib.ts), [key storage](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/entities/messenger/chat-key-storage-idb.lib.ts), [E2EE protocol](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/docs/E2EE.md). | IN PROGRESS for the [native provider, durable vault, reader and scoped issuer](NATIVE_DESKTOP_CRYPTO.md). Local source-client/TLS fixtures cover exact-generation requests, correlated Grant/Reject, historical/nullable generations, archive access, safe Copy and selected/verified-default-topic encrypted text with frozen delivery identity and explicit recovery. Grouping unmounts collapsed bodies, preserves per-UUID manual state and accepted-history/read boundaries; remaining physical selection/focus entrypoints stay open. V33 schema4 adds owner-scoped bounded Body lookup and at-most-four indexed History fallbacks without visible R insertion. Only mounted encrypted G/J demand the linked R; same-R work coalesces, genuine disclosure controls retry and current realtime captures fence older results. V34 adds ordinary/account public recipient preclassification before groups without a Vault, preserves own-author relevance, and resumes unfinished lookups after verified WSS catch-up without automatically retrying settled failures. Its existing resource bounds remain. The V34 Core and complete encrypted UI suites pass 246 and 124 cases, with focused Main and capture groups passing 5 and 4 Qt results; current Controller739, Window221, Runtime10, locale14 and Markdown21 also pass. Cache83 and Service53 retain exact applicable production/test inputs. Matched source expanded response comparisons use exact public inputs and actual Source quote-loader/cache; the encrypted scene uses a synthetic state adapter, while the ordinary/account scene uses a null crypto session and real resolver/IndexedDB/Space collapse. These do not accept Source crypto, unknown participant counts, full shell or live service. Ordinary/provider key issuance, bootstrap/rotation, full issuer recovery, durable outgoing messages, encrypted media/edit/reply/forward and key-management screens remain open. The current V34 staged package passes dependency/TLS/crypto checks and anonymous Wayland startup, single-instance activation and quit. Authenticated package/live and complete cross-platform parity remain open. |
| Calls | Conference-link creation, incoming accept/decline, active call, participant count, PiP/expanded/fullscreen and screen sharing. [Call shell](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/features/jitsi-call/jitsi-call-shell.ui.tsx), [conference host](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/features/jitsi-call/jitsi-call.ui.tsx), [desktop screen sharing](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/features/jitsi-call/jitsi-call-screen-sharing.lib.ts). | NOT IMPLEMENTED |
| Topic summaries and AI surfaces | Topic summary display and permission-scoped configuration; composer AI surfaces have mock/optional HTTP providers and need a separately established live provider. [Topic summaries](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/widgets/right-panel/right-panel-topic-summary.ui.tsx), [summary settings](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/features/topic-summary-settings), [AI providers](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/features/ai-reply/ai-reply.api.ts). | NOT IMPLEMENTED |
| Preferences and accessibility | English/Russian, palette and light/dark/system mode, physical-key shortcut customization, layout/density/sorting, sounds, idle timeout, keyboard/focus and accessible labels. [Settings](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/features/settings/settings.types.ts), [themes](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/features/theme-picker), [hotkeys](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/shared/lib/hotkeys), [settings UI](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/widgets/right-panel/right-panel-user-settings.ui.tsx). | IN PROGRESS for persisted owner-scoped palette/mode, Qt system-appearance resolution, global sidebar width and initial accessibility safety; complete preferences, translations, migration and platform accessibility acceptance remain open. |
| Desktop shell and delivery | Window state, single-instance behavior, tray/close behavior, dock/taskbar badges and progress, native notifications, clipboard, text editing, external URLs, `ew://` links, start-on-login, sleep/resume, call sleep inhibition, diagnostic logs, installers and updates. [Electron main](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/electron/src/main.ts), [bridge](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/electron/src/preload.ts), [packaging](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/electron/electron-builder.yml). | IN PROGRESS for window persistence, single-instance activation and tray close/hide/restore/keyboard quit on the current Wayland host. Published0.1.0 per-user installation and the public tray update-check command passed on that host. The Source-shaped About dock, automatic download and explicit install/restart policy passed local actual Main and component checks. Actual public 0.1.0 → 0.1.1 anonymous installed upgrade passed on the current host with one synthetic History cache row preserved; authenticated account/history and secure-store upgrade remain unaccepted. Physical tray-icon activation, notifications, URI integration and remaining OS workflows are unaccepted. See [published upgrade checks](provenance/linux-published-0.1.1-upgrade-checks.json). See [About/update checks](provenance/about-update-checks.json). |
| Realtime and local recovery | REST catch-up plus WebSocket cursor, deduplication/order, optimistic outbox and delivery, durable cache restoration, reconnect and background-account state. [Realtime](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/shared/lib/workspace-realtime), [outbox](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/entities/messenger/messenger-outbox.model.ts), [cache](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/shared/lib/workspace-messenger-cache-db.ts). | IN PROGRESS for epoch/snapshot/catch-up/WebSocket transport and selected event reducers; durable restoration, complete event domains and live acceptance remain open. |

## Existing limits to reproduce honestly

- [Mail](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/pages/mail/mail-page.ui.tsx) and
  [Calendar](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/pages/calendar/calendar-page.ui.tsx) resolve a
  configured embed URL or a placeholder/unavailable state. Their navigation entries
  are present in [top-bar configuration](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/widgets/top-bar/top-bar.lib.ts).
  They are not evidence of implemented Workspace Mail/Calendar CRUD workflows.
- The standalone [recent-calls page](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/pages/calls/calls-page.ui.tsx)
  displays an empty state. It is separate from the live Jitsi call workflow.
- [Services](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/pages/services/services-page.ui.tsx) contains planned
  service cards. Calls/Services navigation visibility is controlled by build settings.
- Saved snippets and scheduled sending are explicitly unsupported by the live
  Workspace composer. Encrypted chat composition also disables mentions. See
  [chat capabilities](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/pages/chat/chat-page-workspace.ui.tsx).
- Reactions activity displays an unsupported state in the
  [activity page](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/pages/activity/activity-page.ui.tsx).
- AI provider selection and backend availability must be demonstrated separately;
  a mock-generated response is not acceptance of a live AI integration.

## Critical compatibility requirements

1. Use UUIDs and the Workspace/IAM API without legacy numeric-ID conversion or
   hidden provider calls. Preserve permission failures, conflicts, expired sessions,
   invalid routes, default-topic rules, archived/closed states and explicit errors.
2. Reject stale async writes after account/project changes, logout, refresh and
   reconnect. Check ownership for stores, cache, requests, outbox, downloads,
   unread state and notification clicks. No cross-account data may remain visible.
3. Match the rendering protocol, including escaping/sanitization, nested
   quotes/forwards, user/file/time URNs and unsupported media. Do not silently
   discard structured content or render encrypted bytes as readable messages.
4. Preserve E2EE wire compatibility and authenticated context binding. Use protocol
   fixtures and cross-client encrypt/decrypt/key-grant tests, not a new incompatible
   crypto format. Define safe local key storage and migration before claiming
   encrypted history or multi-device access.
5. Implement server-confirmed reads and delivery. Native window focus, viewport
   visibility and notification activation must not mark unrelated messages read.
   An optimistic send or provider-neutral acceptance does not prove external
   provider delivery.
6. Resume realtime from the correct cursor after loss, suspend or network changes;
   reconcile REST and WebSocket results without gaps or duplicate side effects.
   Preserve outbox retry/idempotency behavior and cached-history ownership.
7. Validate files and authenticated media/download requests. Account switches,
   retries, cancellation, expired tokens, missing files and filename handling must
   have explicit outcomes. A selected file is not a completed upload.
8. Accept calls against the configured meeting server, including camera/microphone
   permission failure, incoming-call lifecycle, screen sharing, PiP, sleep/resume
   and cleanup. A visible conference page is not proof of audio/video delivery.
9. Keep provider credentials inside the authorized account-connection flow. External
   integrations require end-to-end message/file delivery and permission checks;
   Workspace persistence alone is insufficient evidence.

## Acceptance plan

Use deterministic synthetic fixtures for automated transport/domain tests and
synthetic test accounts for cross-client scenarios. Do not place credentials,
production messages, personal attachments or unrestricted dumps in fixtures.

For each defect, add a regression test that fails for the actual defect before the
fix and passes afterward. Choose the test level where it occurs: transport/domain,
native widget interaction or end-to-end. A lower-level test does not replace the
user scenario. Existing React tests document behavior but do not execute or accept
the native implementation.

Common acceptance on every operating system must cover:

- Build and start the packaged application from a clean installation; exercise
  keyboard and pointer operation, display scaling, focus, resizing and themes.
- Sign in with and without OTP, select projects, refresh an expired token, cancel
  login, log out and switch accounts during in-flight requests. Check credential
  storage and removal without exposing values.
- Create/use native and bridged conversations; load older/newer history; send,
  reply, edit, delete, forward, react, upload/download and navigate permalinks.
  Verify results through another supported client and server read-back.
- Exercise reconnect, offline/startup cache, cursor catch-up, duplicate/out-of-order
  delivery, suspend/resume, retries and account isolation. Confirm restored drafts
  and read positions without losing newer local edits.
- Exercise notification policy and activation, tray/background operation, badges,
  `ew://` links from a cold and warm start, single-instance behavior and clipboard.
- Cross-test encryption and key exchange with the existing desktop client, and
  calls/media with another participant. Verify provider delivery at the external
  endpoint when applicable.
- Check English/Russian text, Unicode/IME entry, physical-key shortcuts, keyboard
  traversal and assistive-technology labels; include large histories and lists.
- Install an update, restart, and verify settings/data/credential ownership survive;
  prove orderly exit and cleanup of this application's network/media resources.

| Platform | Required platform-specific acceptance | Current result |
| --- | --- | --- |
| Windows | Supported x64/arm64 builds; installer and application identity; notifications and activation; taskbar badge/progress; tray and close/reopen; native file dialogs/clipboard; URI registration; start-on-login; microphone/camera/screen capture; update/relaunch; Narrator and display scaling. | NOT RUN |
| macOS | Supported arm64/x64 builds; bundle identity, signing/notarization and install/launch; menu/Command shortcuts; dock/tray and application activation; notification permission/click; `ew://` open-url; login item; microphone/camera/screen-recording permission; update/relaunch; VoiceOver and display scaling. | NOT RUN |
| Linux | Supported x64/arm64 builds and intended distribution packages; desktop entry/application identity; URI registration; X11 and Wayland sessions where supported; notification activation and tray availability/fallback; clipboard/file dialogs; portal/screen-capture permissions; package/update behavior; keyboard accessibility and display scaling. | NOT RUN |

A platform can be accepted only with its actual build, package, native interaction
and live integration results. Unsupported environments or unavailable checks must
be marked `BLOCKED` or `NOT RUN` with their scope. Full desktop parity remains open
until every implemented baseline scenario and its native integration have passed
on Windows, macOS, and Linux, and baseline unsupported states are represented
accurately.

### Canonical topic links

[Native topic navigation](NATIVE_DESKTOP_TOPIC_NAVIGATION.md) resolves unknown
topics through their owned canonical placement and preserves local state across
failed or superseded lookups. Known topics open without a resolver. Local HTTP
and actual Qt message-link input regressions cover this route; deployed backend
alias behavior and browser history remain separate acceptance requirements.
