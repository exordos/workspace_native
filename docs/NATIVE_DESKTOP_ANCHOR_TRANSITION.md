# Anchor jump: skeleton preview zones and retryable failure overlay

Issue #107 (part of #105). Design fidelity: parity-preserving (docs/NATIVE_DESKTOP_PARITY.md).
Reference: workspace_ui `61dacef78dab2f5d0be76f283bebf49fe29b84af`,
`packages/web/src/widgets/workspace-message-list/workspace-message-anchor-transition.ui.tsx`.
Figma token unavailable - mockups not consulted.

## Reference contract

Mounted as an absolute `inset-0 z-base bg-bg` layer over the message list while a
message-link anchor is being resolved
(`packages/web/src/pages/chat/chat-page-workspace.ui.tsx:4320-4339`,
`data-message-anchor-preview-layer`).

- Pending phase (`workspace-message-anchor-transition.ui.tsx:122-131`): skeleton rows
  appear only after a 100 ms delay keyed to the navigation `intentId` (an instant
  resolution never flashes skeletons). An `sr-only` `role=status aria-live=polite`
  span carries `chat.loadingMessageContext` = "Loading message context" (:143-147).
- Layout (:150-194): vertical flex, `justify-center gap-4` (16 px), padding
  `px-4 py-6` (16/24 px) for the no-preview state. A top zone
  (`data-skeleton-area=top`, `min-h-24` = 96 px, `justify-end gap-3` = 12 px) holds two
  bubble skeletons; a centered preview article (`max-w-2xl` = 672 px) shows the resolved
  preview bubble or an empty `min-h-20` = 80 px placeholder while the lookup is still
  running; a bottom zone (`min-h-24`, `gap-3`) mirrors the top one.
- Bubble skeleton (`workspace-message-bubble-skeleton.ui.tsx`): row `min(72%, 32rem)`
  = min(72 %, 512 px) wide, radius 18 px, padding 12 px, background `border-subtle` at
  40 % alpha, three 12 px bars (`h-3`) at 1/3, full and 2/3 width with 8 px spacing;
  bars are `border-subtle` at 60 % alpha with Tailwind `animate-pulse` (opacity
  1 -> 0.5 -> 1 over 2 s).
- Reduced motion (:29-35): `prefers-reduced-motion: reduce` disables the pulse; the
  static skeletons stay.
- Failed phase (:195-227): inside the same layer, a `flex-col items-end gap-2` block at
  `inset-x-4 bottom-4` (16 px insets) holds a `role=alert` bar - `rounded-lg` (8 px),
  1 px `border-subtle`, `bg-bg-elevated`, `px-3 py-2` (12/8 px), `text-sm` (14 px)
  `text-notice-base` - with the title `chat.messageNavigationError`
  = "Could not open the selected message" (`font-medium`) and a `text-xs` (12 px)
  second line `errorDetail ?? chat.messageNavigationUnavailable`
  = "The selected message is no longer available in the current workspace." On the right
  a retry button `bg-accent` `rounded-lg` `px-3 py-1.5` `text-xs font-medium text-black`
  with `chat.retryMessageNavigation` = "Retry opening message", plus an inline
  scroll-to-bottom button (`onTailNavigationRequested`): jumping back to the conversation
  tail dismisses the failed anchor presentation.
- Strings, en/ru (`packages/web/src/i18n/locales/{en,ru}.json:458-462`):
  `messageNavigationError` "Could not open the selected message" /
  "Не удалось открыть выбранное сообщение"; `messageNavigationUnavailable`
  "The selected message is no longer available in the current workspace." /
  "Выбранное сообщение больше недоступно в текущем рабочем пространстве.";
  `loadingMessageContext` "Loading message context" / "Загружаем контекст сообщения";
  `retryMessageNavigation` "Retry opening message" / "Повторить открытие сообщения".

### Palette (blue-cold default, `packages/web/src/shared/lib/themes/blue-cold.ts`)

| token | dark | light |
| --- | --- | --- |
| `bg` | `#141517` | `#e4ecf3` |
| `bg-elevated` | `#222328` | `#ffffff` |
| `border-subtle` | `#33384d` | `#d8e4ef` |
| `accent` | `#7087ff` | `#7087ff` |
| `on-accent` | `#141517` | `#1b1b1d` |
| `notice-base` | `#3d5eff` | `#7087ff` |

## Native design

Today the native client only shows a `qsTr("Loading…")` empty-list status
(`qml/Main.qml`, the `Text` next to `scrollToBottomButton`) and a failed anchor silently
clears to the safe tail position with a `notice` (docs/NATIVE_DESKTOP_MESSAGE_LINKS.md
"Anchor, highlight and safe landing"). This issue replaces the silent fallback during a
message-link anchor jump with the reference transition layer.

### Controller (`src/native-controller.{h,cpp}`)

The state machine stays untouched; the existing pending-anchor members are exposed:

- `anchorTransitionPending` (bool, `NOTIFY anchorChanged`): true only while a
  **message-link** anchor is unresolved (`applyMessageRoute` / retry path set
  `m_pendingAnchorMessage`; the service conversation-open automatic anchor keeps it out
  of scope via `m_anchorLinkPending`).
- `anchorNavigationError` (QString, `NOTIFY anchorNavigationErrorChanged`): non-empty
  detail text after a failed link resolution (`clearAnchor(notice)` records the notice
  when it cancelled a pending link jump; lookup miss and bounded-walk exhaustion are both
  such calls). Any plain `clearAnchor()` (user navigation, new link, retry, tail
  escape) clears it.
- `retryAnchorNavigation()` re-issues the exact lookup + bounded walk for the failed
  target uuid (fresh budget), mirroring the reference `onRetry`.
- `dismissAnchorNavigation()` clears the failed state (reference tail navigation).
  The safe landing (conversation stays open at its loaded position) is unchanged.

### QML (`qml/Main.qml`)

An `anchorTransitionLayer` `Item` parented to `messageList` (viewport child, like
`scrollToBottomButton`), filling the list area with `root.colors.bg`, visible while
`anchorTransitionPending || anchorNavigationError.length > 0`:

- 100 ms `Timer` keyed to each `anchorChanged` pending pulse gates the skeleton rows
  (`anchorSkeletonsVisible`); a jump that resolves inside 100 ms never shows them.
- Column, spacing 16, margins 16/24: top zone `min 96 px` with two bubble skeletons
  (start, end), centered 80 px placeholder (`max-w 672 px`) - the native list has no
  out-of-window preview bubble yet, so the reference `data-message-preview-placeholder`
  empty state is the standing look - and a bottom zone `min 96 px` with two bubble
  skeletons (end, start). Skeleton bubbles follow the reference geometry
  (min(72 %, 512 px), radius 18, 40 % `border-subtle` panel, 60 % bars, 12 px rows).
- Pulse: `OpacityAnimator` 1 -> 0.5 -> 1, 2 s infinite, `running:
  shimmerActive && anchorSkeletonsVisible`; `shimmerActive` is `!root.prefersReducedMotion`.
  `prefersReducedMotion` is bound to `NativePreferences.prefersReducedMotion`, the
  native stand-in for the CSS media query: Qt 6.10 has no reduced-motion hint
  (`QStyleHints::accessibility()` only carries contrast), so the preferences object
  reads the XDG Settings portal `org.freedesktop.appearance/reduced-motion` key at
  startup (service `org.freedesktop.portal.Desktop`, object path
  `/org/freedesktop/portal/desktop`, interface `org.freedesktop.portal.Settings`,
  `Read`, 2 s timeout, unknown value = no preference per spec) and
  keeps it live via the `SettingChanged` signal on that same addressing. The test
  harness pins the same value through that seam. The live-region `Text`
  "Loading message context" renders in place of the old
  bare "Loading…" status while a link jump is pending (the generic empty-conversation
  "Loading…" text stays for non-anchor empties). QML has no live-region attach
  property, so the sr-only text carries `Accessible.role: Accessible.StaticText`
  with its name, matching how the repo exposes other status lines.
- Failed: `anchorErrorOverlay` at 16 px insets from bottom/sides, radius 8, 1 px
  `border-subtle`, `bg-elevated`, 12/8 px padding, `Accessible.role: AlertMessage`;
  title "Could not open the selected message" (14 px Medium `notice-base`), detail line
  (12 px) with the controller notice; `anchorRetryButton` (`bg accent`, radius 8,
  12/6 px padding, 12 px Medium `on-accent` text "Retry opening message") calls
  `controller.retryAnchorNavigation()`; below it an inline `anchorScrollToBottom`
  button (same look as the existing floating scroll-to-bottom) calls
  `controller.dismissAnchorNavigation()` + `messageList.scrollToLatest()`.

A failed link and its Retry action belong to the stream and topic where resolution
failed. Selecting another conversation dismisses the alert. While either the pending
or failed layer covers the viewport, the list suspends automatic read receipts.
Dismissing the layer resumes the normal visible-row and end-of-topic checks.

### Deterministic tests

The mock server `Response.defer` gate (tests/mock-server.h) holds the
`GET /messages/?uuid=` lookup until the test releases it - pending and failed phases are
triggered by explicit releases, never by sleeps racing the network. Window cases
(`tests/native-window-test.cpp`): skeleton zones appear after the delay while a link is
held pending (96 px zones, empty 80 px placeholder - no skeleton bubble inside it),
live-region text), reduced-motion keeps the zones static (`shimmerRunning: false` and
the `anchorSkeletonPulse` animation not running) with the flag driven through the
preferences portal seam, a failing lookup shows the alert with title/detail
and Retry, clicking Retry re-issues the lookup (second request observed, overlay replaced
by the pending skeleton layer, alert returns when the bounded walk lands safely), and the
scroll-to-bottom button dismisses the overlay.

## Locales

New `qsTr` strings in `Main` context are added to
`packages/native/translations/workspace_ru.ts` with the reference ru strings above; the
lupdate completeness gate stays green.
