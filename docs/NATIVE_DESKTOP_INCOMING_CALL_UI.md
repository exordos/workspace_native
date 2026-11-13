# Native desktop incoming call: large and compact invite UIs with ringtone schedule (#163)

Status: implemented. Part of #33. Builds on the merged call stack docs:
[NATIVE_DESKTOP_CALL_STACK.md](NATIVE_DESKTOP_CALL_STACK.md) (#160),
[NATIVE_DESKTOP_CALL_LAUNCH.md](NATIVE_DESKTOP_CALL_LAUNCH.md) (#161 store +
meet host) and [NATIVE_DESKTOP_CALL_WINDOW.md](NATIVE_DESKTOP_CALL_WINDOW.md)
(#162 call window shell). This issue adds the *incoming invite*: ingest from
unread DM messages carrying a meeting URL, the large and compact invite UIs,
the ringtone schedule and the 45 s auto-decline.

## Reference contract

All measurements are from `workspace_ui` at
`61dacef78dab2f5d0be76f283bebf49fe29b84af` (read-only clone; sha present).
Line numbers below were re-measured from that sha at implementation time.

### Ingest filters — `workspace-jitsi-incoming-call.lib.ts:40-73`

An invite is built from a message only when: the message is not read, is not
own and its author is not the current user; the stream exists, is private and
has a `direct_user_uuid`; the payload kind is `markdown`; and the body yields a
meeting URL through `getJitsiMeetingUrl` (configured Workspace meet host first,
`meet.jit.si` always accepted — `shared/lib/jitsi.ts:30-65`). Caller name
falls back display name -> trimmed stream name -> first 8 chars of the author
uuid (`:23-34`). The invite carries `messageId`, `meetingUrl`, `callerName`,
`locationName` (stream name), `ownerKey`, `meetUrl`, `displayName`,
`avatarUrl` and `timestamp`. The realtime hook wires this on every created
message (`layout-workspace-realtime.hook.ts:448-465`).

Store-side rules (`jitsi-call.model.ts:136-159`, matrix from
`jitsi-call.model.test.ts:172-231`): dedupe by `messageId` against
`lastIncomingMessageId` (dedupe hit does not even record a new id); while an
active call exists the ingest is ignored (the id is still remembered); while an
invite is already visible a second invite never replaces it. Accept with an
active call clears the invite and keeps the original call. `JitsiCallStore`
already shipped these rules with #161.

### Large variant — `jitsi-call-incoming-large.ui.tsx:13-20, 43-50, 85-142`

- Full-viewport layer, `p-4`; scrim `bg-black/55` (black 55 %).
- Section: `918x574`, max `calc(100vw-2rem)/calc(100vh-2rem)`, `rounded-2xl`
  (16 px radius), background `#000000`, `px-6 py-8 sm:px-12 sm:py-10`.
- Avatar: `160 px`, `200 px` at the `sm` (640 px) breakpoint. Image avatar
  when `avatarUrl` is present; otherwise the first letter
  (uppercase, `text-6xl` 60 px semibold) or a `48 px` phone glyph when the
  title is empty, on `bg-neutral-500` (`#737373`, Tailwind default scale —
  the reference tailwind config defines no neutral override; gap: the
  contract names the token, the hex was measured from the default scale).
- Caller name 24 px (`text-2xl`) semibold white; subtitle
  `call.incomingChooseMethod` 18 px (`text-lg`) `#99ffffff` (white/60).
  `inviteTitle` falls back to `call.participant` when the caller name trims to
  empty (`jitsi-call-shell.ui.tsx:37-38`).
- Button group: max width 514 px, gap 8 px, buttons height 52 px,
  `rounded-lg` 8 px, inner icon slot 40 px (`INCOMING_CALL_ICON_SLOT_PX`,
  `:13`), inner gap 6 px. Figma button group 6176:36282.
  - `With video` (checkbox-role toggle): on `bg-call-green #26c038` + black
    text, off outlined `bg-bg` + `border-border-subtle` + `text-text-muted`;
    the video icon renders at 40x40 (`call_incoming_video_toggle.svg`).
  - `Cancel`: `bg-call-red #e43535`, white text, close glyph 20 px
    (`INCOMING_CALL_CLOSE_GLYPH_PX`, `:15`) in a 40 px slot (`:14`).
  - `Accept`: `bg-call-green #26c038`, black text, phone glyph 27 px
    (`INCOMING_CALL_PHONE_GLYPH_PX`, `:17`) in a 40 px slot.
- Per-invite video preference keyed by `messageId`, default `false`
  (`jitsi-call-shell.ui.tsx:41-44, 61-74`); accept maps
  `startWithVideoMuted = !videoEnabled` and clears the preference; decline
  clears it too.

### Compact variant — `jitsi-call-incoming-compact.ui.tsx:25-73`

- Toast fixed `right-4 top-20` -> x-offset 16 px from the right edge,
  y = 80 px; width 320 px (max `calc(100vw-2rem)`), `rounded-xl` (12 px),
  `bg-bg-elevated`, `border-border-subtle`, `p-3` (12 px), shadow-xl.
- Row gap 8 px, `mb-2` to the button row: sm avatar 32x32 (`Avatar size="sm"`
  = `w-8 h-8 text-xs`, `shared/ui/avatar/avatar.tsx:8`; gap measured from
  source — the contract gives no size); image avatar, else a 16 px phone
  glyph on `call-green/15` with `text-call-green` foreground.
- `call.incomingCall` 14 px (`text-sm`) semibold `text-text-primary`;
  `call.incomingFrom` 12 px (`text-xs`) `text-text-muted`.
- Buttons right-aligned gap 8 px: ghost sm `call.decline` (sm = `h-8 px-3
  text-xs`, `button.tsx:9`; neutral ghost = transparent + `text-text-muted`,
  `button.tsx:33`) and filled sm `call.accept` (primary filled =
  `bg-accent text-on-accent font-semibold`, `button.tsx:17`).

### Lifecycle — `jitsi-call-incoming-lifecycle.hook.ts:6-8, 21, 28-58`

Visible while `incomingInvite != null && activeCall == null`. Auto-decline:
single-shot at exactly `45000 ms` calling the decline handler (clears the
invite state; no auto-accept ever). Ringtone: plays immediately on becoming
visible then every `1500 ms` while visible; suppressed entirely when the
notification-sound preference equals `"none"`. Preset `soft_call`.

### Variant flag — `jitsi-call-shell.ui.tsx:45`, `shared/lib/env.ts:132-135`

`VITE_CALL_INCOMING_MODAL_VARIANT` default `large`; only `compact`
(case-insensitive) selects the compact UI, anything else falls back to
`large`. Both variants must exist and be reachable.

### Palette tokens (theme `blue-cold` default; light / dark)

`call-green` `#26c038` / `#26c038`; `call-red` `#e43535` / `#f04c4c`;
`bg-elevated` `#ffffff` / `#222328`; `border-subtle` `#d8e4ef` / `#33384d`;
`text-primary` `#1b1b1d` / `#ffffff`; `text-muted` `#989898` / `#707070`;
accent `#7087ff`; on-accent `#1b1b1d` / `#141517`
(`shared/lib/themes/blue-cold.ts:8-27, 68-84, 104-106`). The large card is
`#000000` in both themes (`jitsi-call-incoming-large.ui.tsx:49`). The native
`NativeTheme.qml` palette map already carries every token above; the QML uses
token lookups, never hardcoded theme colors, except the fixed black card and
white/60 text which are theme-independent in the reference.

### i18n (locales `en.json`/`ru.json` at the sha, keys `call.*`)

`incomingCall` "Incoming call"/"Входящий звонок"; `incomingFrom`
"{{name}} is calling"/"{{name}} звонит" (Qt: `%1`); `accept` "Accept"/
"Принять"; `decline` "Decline"/"Отклонить"; `cancel` "Cancel"/"Отменить";
`withVideo` "With video"/"Вкл. видео"; `incomingChooseMethod`
"Choose how to answer the call"/"Выберите удобный способ для звонка";
`participant` "Participant"/"Участник". Every new `tr()`/`qsTr()` string
ships its reviewed Russian entry in `packages/native/translations/workspace_ru.ts`
(guarded by `native-native-locale`).

## Native design

- `packages/native/src/call/incoming-call.h/.cpp` — pure functions:
  `buildIncomingDmCallInvite(input map)` (port of
  `buildWorkspaceIncomingDmCallInvite`, same filters and fallback chain; empty
  map = no invite) and `resolveIncomingCallVariant(raw)` (port of
  `env.ts:132-135`: lowercase, `"compact"` -> `compact`, anything else ->
  `large`).
- `packages/native/src/call/incoming-call-lifecycle.h/.cpp` —
  `workspace::IncomingCallLifecycle`, a GUI-free state machine bound to the
  `JitsiCallStore`:
  - `visible` = invite present and no active call (recomputed on the store
    signals).
  - `ringIntervalMs` (default 1500, `jitsi-call-incoming-lifecycle.hook.ts:7`)
    and `autoDeclineMs` (default 45000, `:6`) are injectable so tests drive
    the cadence with shortened values while the production defaults stay
    exactly at the reference constants; `visibleChanged` (re-)arms both
    timers.
  - `ringRequested()` is the audible seam: emitted immediately on becoming
    visible then every `ringIntervalMs` while visible, and suppressed while
    `notificationSound == "none"` (reference `:43-46`). Tests assert the
    schedule via `QSignalSpy` — no audio device is involved.
  - At `autoDeclineMs` the controller runs the decline handler (clears the
    per-invite video preference, `store.declineIncomingInvite()`) and emits
    `autoDeclined(messageId)`; there is no auto-accept anywhere.
  - Per-invite video preference (`videoEnabled(messageId)`,
    `toggleVideoEnabled(messageId)`, cleared on accept/decline) and the
    `accept()`/`decline()` handlers; `accept()` calls the new
    `JitsiCallStore::acceptIncomingInvite(options)` overload with
    `{startWithVideoMuted: !videoEnabled}` (default muted).
  - `variant` resolves the compile-time `WORKSPACE_NATIVE_CALL_INCOMING_VARIANT`
    (CMake cache `WORKSPACE_CALL_INCOMING_MODAL_VARIANT`, default `large`)
    through `resolveIncomingCallVariant`; writable so the window suite can
    exercise the compact toast without two builds.
- `JitsiCallStore` gains the `acceptIncomingInvite(QVariantMap options)`
  overload (`startWithVideoMuted` honored, default `true` preserved); the
  existing no-arg overload keeps its exact behavior.
- `packages/native/qml/NativeIncomingCallWindow.qml` — one component,
  `variant` property drives the large card or the compact toast; props:
  `invite` (store map), `colors`, `videoEnabled`, signals
  `acceptRequested/declineRequested/toggleVideoRequested`. Geometry follows
  the measurements above with objectNames `incomingCallScrim`,
  `incomingCallCard`, `incomingCallTitleText`, `incomingCallSubtitleText`,
  `incomingCallVideoToggle`, `incomingCallDeclineButton`,
  `incomingCallAcceptButton`, `incomingCallToast` etc. Glyphs reuse the
  bundled reference SVGs (`phone.svg`, `close.svg`,
  `call_incoming_video_toggle.svg` added to the icon resource list) sized to
  the contract glyphs, not the outer slots.
- `Main.qml` — `incomingCallOverlay` above the app content (same pattern as
  the #162 call overlay): large = full-bleed scrim + centered card; compact =
  320 px toast anchored `x = width - 320 - 16, y = 80` (the reference
  `right-4 top-20`). Buttons route through `controller.incomingCall`.
- Wiring in `NativeController`: exposes `incomingCall` (the lifecycle
  controller). `applyRealtimeEvent` calls `maybeOfferCallInvite(message)` on
  every created message (reference `onMessageCreated`), which resolves the
  stream row (private + direct user), the author display name via
  `userDisplayName`, the current user and the effective meet host, runs
  `buildIncomingDmCallInvite` and feeds `JitsiCallStore::ingestIncomingInvite`.
  The store keeps the dedupe/ignore matrix, so replayed or duplicate events
  are inert.

## Deviations recorded

- Ringtone audio: the reference synthesizes the `soft_call` preset with Web
  Audio oscillators (`shared/lib/notification-sound.ts`). Qt6 Multimedia is
  not available on this host, so the native client ships the exact ring
  schedule as the `ringRequested` signal (the testable seam the issue's
  acceptance criteria use — "assert on state/timer signals") with no audible
  tone yet; the `notificationSound == "none"` gate is implemented. When a
  native audio sink lands, it attaches to this signal without behavior change.
- Large avatar width: the reference switches 160 -> 200 px at the 640 px
  CSS breakpoint; the native overlay reproduces the breakpoint against the
  window viewport (`viewport >= 640 -> 200 px`, else 160 px), so a desktop
  window renders the 200 px wide value exactly like the reference browser.
- Variant flag: build-time CMake cache variable instead of `VITE_*`; the
  resolution function is a byte-exact port (default large, invalid -> large).
- The notification-sound preference itself has no native settings surface yet;
  `notificationSound` is an injectable string property on the lifecycle
  controller (default `"default"`, gate on `"none"` exactly like `:44`).
- Ingest source: the reference builds the invite inside the realtime applier
  per created message; native reuses the already-parsed DTO in
  `applyRealtimeEvent` (same envelope owner/project guards as every other
  realtime feature), so encrypted streams (payload kind is not `markdown`
  before decryption) never raise an invite, matching the reference filter.
- Compact avatar tint: the reference paints the no-photo avatar with
  `color-mix(in oklab, call-green 15%, transparent)`; QML has no oklab mix,
  so the native avatar uses `Qt.alpha(call-green, 0.15)` (same color, same
  15 % strength, different mix space - visually identical on the
  `bg-elevated` backdrop it sits on).
- The "With video" checkbox glyph is the reference asset itself
  (`call_incoming_video_toggle.svg`, a 40 px stateful icon) rendered raw at
  the 40 px icon slot exactly like the web `<IncomingCallVideoIcon>`; no
  separate checkbox control is drawn.
- Dismiss: Escape clears the invite without a decline action
  (`IncomingCallLifecycle::dismiss`, matching the reference
  `useEscapeListener` -> "incoming call dismissed without invite"). The
  large variant's scrim is a dismiss affordance exactly like the reference
  `closeOnBackdrop: true` (large.ui.tsx:43); the compact toast renders
  without a backdrop, so it exposes Escape only.

## Tests

- `native-native-call` (offscreen): the ingest filter matrix (read/own/self
  author, non-private stream, non-markdown payload, foreign hosts accepted
  only on the configured host and meet.jit.si), caller-name fallback chain,
  invite field mapping; dedupe-by-id, ignore-while-active and
  keep-first-involve store cases (mirror of `jitsi-call.model.test.ts`);
  accept option overload (default muted, toggle -> unmuted); variant
  resolution (default/compact/COMPACT/garbage -> large semantics); lifecycle
  constants 1500/45000, immediate ring then repeated at the injectable
  interval, `none` gate silence, auto-decline fires decline + clears invite
  on the injectable clock and never opens a call, hiding (accept/decline/
  active call) cancels both timers, per-invite video preference lifecycle.
- `native-native-controller`: a realtime `message.created` on a private DM
  raises the invite (caller display name), a foreign-author-name-less sender
  falls back, a second delivery of the same id is deduped, nothing raises
  while a call is active or the message is not markdown, and accepting
  through `incomingCall.accept()` opens the active call with the video flag
  the toggle set.
- `native-native-window` (Xvnc): large overlay geometry at 1280x800 — scrim
  55 % black, card 918x574 centered radius 16 black, title 24 px semibold
  white, subtitle 18 px white/60, button row width 514 with 52 px buttons
  and 8 px gaps, accept/decline colors from the palette; compact toast 320 px
  at x=window-16-320, y=80; clicking Accept opens the call with
  `startWithVideoMuted` toggled per preference; clicking Cancel/Decline
  clears the invite; auto-decline clears the invite (short injectable clock);
  all strings resolve in en and ru.
