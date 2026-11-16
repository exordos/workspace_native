# Direct-chat header: deactivated-account badge (issue #177)

Reference: workspace_ui at `61dacef78dab2f5d0be76f283bebf49fe29b84af`.
All geometry below was measured from the reference files at that sha, not
estimated.

## Reference contract

- `packages/web/src/widgets/chat-view/chat-header-direct.ui.tsx:34-42` renders
  the partner avatar as a size `md` (40 px) `WorkspaceAvatar` and passes
  `deactivated={partner.isAccountDeactivated === true}`.
- `packages/web/src/shared/ui/avatar/avatar.tsx:20-23,53,84-95`: when
  deactivated, the presence indicator is swapped for the block badge. The
  badge is positioned with `bottom/right: calc(14.65% - 7px)`. The presence
  dot uses `calc(14.65% - 4px)`. Both formulas put the badge center on the
  avatar circle at 45 degrees: for a 40 px avatar that center is
  `40 * 0.8535 = 34.14 px` from the left/top edge. The existing native
  `PresenceBadge` already encodes this as `x = parent.width * 0.8535 - 6` for
  its 12 px box (center `34.14`).
- `packages/web/src/shared/ui/presence-indicator/presence-indicator.tsx:40-50`:
  the deactivated badge is a `rounded-full bg-card-bg` span with `ring-2
  ring-bg` (a Tailwind box-shadow ring, drawn outside the element box) and a
  14 px `block` glyph in `text-text-muted` (`#989898` light, defined in
  `app.styles.css`). `ring-2` adds 2 px on each side, so the total visual box
  is 14 + 2 + 2 = 18 px. The element carries `role="status"` and
  `aria-label={t("dm.partnerBlocked")}`.
- Locales at that sha: `dm.partnerBlocked` is `"Account deactivated"` (en) and
  `"Пользователь заблокирован"` (ru) - taken verbatim from
  `packages/web/src/i18n/locales/en.json:898` and `ru.json:906`.
- Contract test `chat-header-direct.ui.test.tsx:42-57`: a deactivated partner
  hides the presence dot (`[data-presence]` is absent), shows the status-role
  badge named by `dm.partnerBlocked`, and the status caption replaces the
  presence caption (`chat-header.lib.ts:20-22`).

## Reference data pipeline (important caveat)

At this sha the reference declares `isAccountDeactivated?: boolean` on the
header partner type (`chat-header.types.ts:14`, commented "Whether the user
directory reports a deactivated account") and consumes it in the header and
composer, but no production code path assigns it yet - only tests set it.
There is therefore no upstream JSON field name to copy. This slice documents
the native projection decision explicitly:

- The native controller projects the flag from the same authoritative user
  directory rows it already parses for presence/avatar/status
  (`makeUserPresentation`), reading the optional boolean field
  `account_deactivated` (snake_case, consistent with `status_text`,
  `created_at`, `updated_at`). Missing or non-boolean -> `false`, never
  invented.
- `NativeController::directPeer()` exposes it as `accountDeactivated`
  alongside the existing `displayName`, `avatar` and `presence`. If the real
  API later names this field differently, only this projection changes.

## Native design

- `src/native-controller.{h,cpp}`: `UserPresentation` gains
  `bool accountDeactivated` parsed in `makeUserPresentation` from
  `account_deactivated`; like other replaced facts it is taken from the latest
  row, not kept by the presence `updated_at` guard. A `userAccountDeactivated(id)`
  accessor mirrors `userPresence` gating (authenticated, authoritative
  directory owner). `directPeer()` adds `accountDeactivated`.
- `qml/Main.qml` `directConversationHeader`:
  - `PresenceBadge` receives no presence while the partner is deactivated, so
    the dot is hidden exactly like the reference swap.
  - New sibling badge, mirroring `PresenceBadge` structure: 18 x 18 Item at
    `x/y = parent * 0.8535 - 9` (same center as the presence dot), 2 px ring
    in `colors.bg`, inner 14 px circle in `colors["card-bg"]`, and the
    `block.svg` glyph (already vendored at
    `packages/web/src/shared/assets/icons/block.svg`, added to the `native_icons`
    qrc list) at 14 px tinted `colors["text-muted"]` via the existing
    tinted-icon idiom. `Accessible.role: Accessible.StatusBar` (native
    counterpart of the reference `role="status"`, as used by `PresenceBadge`)
    and `Accessible.name: qsTr("Account deactivated")`.
  - The status caption shows the same localized string instead of the presence
    caption while deactivated, matching `resolveDmStatusText`.
- `translations/workspace_ru.ts`: `Main` context entry
  `Account deactivated` -> `Пользователь заблокирован`. The en value is the
  source string itself.

## Acceptance (native-window-test, native-native-window group)

Data-driven en/ru case on the direct-chat fixture: the partner user row gains
`account_deactivated: true` with presence `active`; the test asserts the
controller projection, that `presence-direct-header` is gone, that the
deactivated badge is 18 x 18 with a 14 px card-bg inner disc and a 2 px bg
ring at the same 14.65 percent inset center as `PresenceBadge`, that its
Accessible name equals the localized string in both locales, and that the
header caption shows the same string. The ordinary presence path is covered by
the unchanged existing presence tests.

## Verification plan

Unit/window: `native-window-test`, `native-controller-test`, `native-locale-test`
(new tr string guard). Live: `live_qa.py native-login` screenshot + vision check
that the ordinary direct header still shows the normal presence dot (the live
workspace has no deactivated partner, so the badge itself is mock-proven at
window level - accepted precedent from issues #222/#243); health p95 and send
latency per the standard live gate.
