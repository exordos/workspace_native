# Native user profile drawer content (issue #134)

Part of #121. This document specifies the profile content layer of the right
drawer: own profile ("Personal info", opened from the account menu) and other
users' profile ("Information", opened from a mention click or the direct-chat
header). It covers the read view, own-profile edit mode, the Edit avatar
dialog, share-link copy and the Message action that opens or creates the
direct chat. It mounts on the drawer shell of #129
(`docs/NATIVE_DESKTOP_RIGHT_DRAWER.md`) as a profile-override screen of the
`chat-info` layer and as the `personal-info` screen of the `account` layer.
The channel-info content of #132 (`NativeChannelInfo.qml`) is the structural
precedent; `NativeRightDrawer.qml` is reused unchanged as the shell.

Design fidelity case: parity-preserving port of the existing Electron/React
surface, no redesign.

Reference contract: workspace_ui `61dacef78dab2f5d0be76f283bebf49fe29b84af`:

- `packages/web/src/widgets/right-panel/right-panel-user-profile.ui.tsx`
  (view header, presence line, edit mode, avatar preview/pending change,
  share copy, Message/Call navigation),
- `packages/web/src/widgets/right-panel/right-panel-user-profile-actions.ui.tsx`
  (action chips, copy state flip),
- `packages/web/src/widgets/right-panel/right-panel-user-profile-details.ui.tsx`
  (detail rows + Copyable),
- `packages/web/src/widgets/right-panel/right-panel-user-profile-edit-avatar-dialog.ui.tsx`
  (Edit avatar dialog),
- `packages/web/src/widgets/right-panel/right-panel-user-profile-direct-navigation.lib.ts`
  (Message/Call target resolution),
- `packages/web/src/features/user-profile/user-profile-editor.lib.ts` (draft,
  display name, patch, validity), `user-profile.api.ts` (IAM load/update,
  avatar capabilities 25 MiB static, avatar upload/reset),
- `packages/web/src/shared/api/workspace-iam-profile.api.ts`
  (`GET|PUT {origin}/api/core/v1/iam/users/{uuid}`),
  `packages/web/src/shared/api/workspace-client.ts`
  (`POST {messenger}/users/{uuid}/actions/avatar_upload/invoke` (multipart
  field `file`), `POST .../actions/avatar_reset/invoke` (`{}` body)),
- `packages/web/src/entities/messenger/messenger-right-panel.lib.ts`
  (`createWorkspaceRightPanelUserDetails`: userId/email/joined from the user
  row; phone, jobTitle, manager, role, accountType, accountStatus, timezone,
  localTime, birthday always temporarily-unavailable),
- `packages/web/src/shared/lib/workspace-profile-link.lib.ts`
  (`buildWorkspaceProfileShareLink`), `shared/lib/validation.ts`
  (`validateFileUpload` 25 MiB/empty, magic-byte image kinds),
- `packages/web/src/shared/ui/copyable/copyable.tsx` (glyph flip 2000 ms),
  `shared/ui/presence-indicator/presence-status-text.lib.ts`
  (active→call-green, idle→indicator-orange, dnd→call-red, offline/null→
  text-muted), `shared/ui/avatar/avatar.tsx` (`xl` = 64 px, `text-xl` initial,
  presence dot `sm` 8 px ring-2),
- contracts: `right-panel-user-profile.ui.test.tsx`, `-details.ui.test.tsx`,
  `-actions.ui.test.tsx`, `-edit-avatar-dialog.ui.test.tsx`,
  `-direct-navigation.lib.test.ts`.

## Measured gaps versus the issue text

Measured from source at the pinned sha, which is the oracle:

- The temporarily-unavailable detail value is
  `workspaceMessenger.temporarilyNotConnected` ("Temporarily not connected" /
  "Временно не подключено"), not the literal "-" the issue cites
  (`messenger-right-panel.lib.ts:65` passed from `right-panel-shell.ui.tsx:65`).
  Native renders that label, italic, `text-text-muted`.
- The issue cites "323x212" for the avatar dialog. The reference shell fixes
  only the width (`w-[323px]`); the Figma 212 height includes the camera row,
  which is hidden at this sha ("Restore the camera action here when taking
  photos is supported"). With gallery + remove only, the measured content
  stack is 20 + 32 + 8 + 32 + 20 + 40 + 20 = 172 px high: native uses a
  323 px fixed width and that derived height.
- The drawer shell already renders Back + title (`right-drawer.ui.tsx`), and
  the reference shell keeps title/back on the shell for profile screens
  ("Dedicated drawer mode: shell owns title + back; profile body has no nested
  header", `right-panel-shell.ui.tsx:69-71` - `onBack` is never passed at this
  sha). Native therefore renders no second header inside the page:
  `rightDrawer.title` carries "Personal info"/"Information" and the shell Back
  button pops the nested screen (same 32x32 geometry as #129).
- Avatar capabilities in the reference are static
  (`getOwnAvatarCapabilities()` -> 25 MiB max, `avatarChangesDisabled: false`,
  `user-profile.api.ts:64,228-232`). There is no org-settings call at this
  sha, so native mirrors the static capability; the capability value still
  flows through one controller property so a later server-driven change is a
  single-point edit.
- The Favorites chip navigates to the activity favorites filter, which is
  tracked by #70 and has no native destination yet; the Call chip is gated on
  the calls epic #33 ("render the Call chip disabled until that epic lands").
  Native renders both chips; Favorites clicks are inert until #70 lands, Call
  renders disabled (`opacity 0.5`).
- The native stream catalog does not carry per-stream topic lists for
  unselected streams, so "open the DM stream default topic" selects the
  stream and lands on the default topic when the topic page verifies
  (pending-route guard); a verified topic list without exactly one default
  topic resolves like the reference `missing-default-topic` branch: log and
  stay.

## Geometry and colors (Tailwind tokens resolved to px)

Palette tokens are the reviewed `blue-cold` values already present in
`NativeTheme.qml`; tests read `colors[token]` and only palette-independent
geometry is asserted numerically. Reference hexes (dark / light):
`card-bg` #282a32 / #eef5fb, `card-bg-active` #2c3747 / #cde6ff,
`bg-elevated` #222328 / #ffffff, `accent` #7087ff both, `on-accent` #141517 /
#1b1b1d, `border-subtle` #33384d / #d8e4ef, `text-primary` #ffffff / #1b1b1d,
`text-secondary` #999999 / #787878, `text-muted` #707070 / #989898,
`icon-base` #707070 / #989898, `icon-active` #ffffff / #1b1b1d,
`call-green` #26c038 both, `call-red` #f04c4c / #e43535,
`indicator-green` #26c038, `indicator-orange` #ff8900, danger text token
`danger` #d92d20 both (`tokens.ts`; the dialog destructive row uses
`call-red`, not `danger` - the error line uses `danger`).
Text sizes: `text-xl`=20, `text-base`=16, `text-sm`=14, `text-xs`=12.

Content scroll area: `py-2` (8 px top/bottom), blocks `space-y-5` (20 px gaps).

View header (`right-panel-user-profile.ui.tsx:563-646`): row `flex items-center
gap-4 px-2` (gap 16, horizontal padding 8). Avatar size `xl` = 64x64,
`bg-bg-elevated text-text-secondary` fallback with the first letter of the
title (`text-xl` 20 px semibold); presence dot `sm` 8 px with a 2 px ring in
the drawer surface color at the bottom-right corner, colors
`indicator-green` (active), `indicator-orange` (idle), `text-muted`
(offline). Presence dot only in view mode (`presence` prop is `null` in edit
mode). Name column `h-16` (64) `flex-col justify-between`: name
`text-xl font-medium leading-6` (24 px line) truncated; optional custom
status line `text-xs leading-4 text-text-secondary`; presence label line
`text-xs leading-4` colored by `resolvePresenceStatusTextClass`
(active→`call-green`, idle→`indicator-orange`, dnd→`call-red`,
offline/null→`text-muted`).

Action chips (`-actions.ui.tsx`): `flex gap-2` of buttons
`rounded-lg bg-card-bg px-2 py-2`, inner `gap-0`, 40x40 icon box
(`Icon text-icon-active`), label `text-xs leading-4 text-text-secondary`,
`hover:bg-bg`, disabled `opacity 0.5`. Self: Favorites (`home` 26, fill),
Edit (`border_color` 30, hug width so "Редактировать" never truncates),
Share (`links` 40). Other: Message (`chat_bubble` 28), Call (`phone` 30),
Share. Copied state: glyph `check` 28, label `message.copied`, 2000 ms.
Self edit mode hides the chip row (`showActions = !(self && editing)`).

Detail rows (`-details.ui.tsx`): list `gap-3` (12 px). Read row: `h-9` (36)
bottom-aligned (`items-end`); left column `gap-3` (12) `items-end`; icon box
32x32 `text-icon-base` glyph 24; label column `gap-1` (4): label
`text-xs leading-4 text-text-secondary` truncate, value `text-sm leading-4
text-text-primary` truncate; unavailable value `italic text-text-muted`.
Trailing copy button 24x24, glyph `copy` 24, `text-text-muted`,
`hover:bg-bg-elevated hover:text-text-primary`, glyph flips to `check` for
2000 ms after a copy (`copyable.tsx`); copy never renders on editing,
unavailable or multiline rows. Row order and icons: userId `alternate_email`,
email `mail`, phone `phone`, jobTitle `business_center`, manager `handshake`,
role `account_circle`, accountType `group`, accountStatus `info`, timezone
`globe_location_pin`, localTime `schedule`, joined `calendar_month`,
birthday `celebration`. `joined` formats the user `created_at` with the
locale's short-month joined style (reference `formatDateJoined`,
`{year: numeric, month: short, day: numeric}`). Values come from the
verified user-directory row exactly like `createWorkspaceRightPanelUserDetails`.

## Edit mode (self only, `right-panel-user-profile.ui.tsx:602-731`)

- Avatar becomes a button with a 20x20 round badge (`bg-icon-base`, `stylus`
  glyph 12 in white) at the bottom-right; it opens the avatar dialog and is
  disabled while a save is in flight.
- Display-name block: label `settings.profileDisplayName`
  `text-xs leading-4 text-text-secondary`, preview box `h-8 rounded-lg
  bg-card-bg px-2` with the draft-derived display name
  (`profileDraftDisplayName`: trimmed "First Last", falling back to the
  username) in `text-xs leading-4 text-text-primary` truncated.
- Hairline `border-border-subtle` between the avatar block and the details.
- Details list gains, in order: firstName and lastName rows (icon
  `account_circle`, icon box `mt-5` top-aligned, editable `ProfileEditorField`
  inputs), a read-only username row (`alternate_email`), the standard detail
  rows with the phone row replaced by an editable field, and the multiline
  description row (icon `info`, textarea 3 rows).
- Editor field: `h-8 rounded-lg border border-border-subtle bg-card-bg px-3
  text-sm leading-4 text-text-primary`, focus ring accent (native: focus
  border accent), disabled `opacity 0.6`; description textarea `py-2`
  wrap.
- Validity (`isProfileDraftValid`): first/last name <= 128 chars with no
  embedded LF, trimmed phone <= 15 chars. Save is disabled while saving or
  while the draft is invalid; the invalid draft also shows
  `settings.profileInvalidFields` as a `role=alert` line.
- Footer `flex gap-2 pb-2`: Cancel fixed 110x40 `rounded-lg bg-card-bg-active
  px-4 text-sm font-medium leading-5 text-accent hover:opacity-90`; Save
  `flex h-10 rounded-lg bg-accent px-4 text-sm font-medium text-on-accent`
  (on-accent #141517 dark / #1b1b1d light).
- Save flow (`handleSaveEdit`): if nothing is dirty, only the pending avatar
  mutation runs; otherwise `PUT /api/core/v1/iam/users/{uuid}` with only the
  changed keys (`first_name`, `last_name`, `phone`, `description`; nullable
  names become null when cleared) first, then the avatar mutation. The
  response re-validates `uuid == session user`. After the update the client
  re-reads `GET {messenger}/users/{uuid}`; a mismatch or failure leaves the
  profile saved but shows `settings.profileRefreshPending` (the reference
  `refreshPending` state) without rolling back.
- Cancel resets the draft, the pending avatar change and the dialog.
- A profile refresh status line `settings.profileRefreshPending` renders under
  the header while pending. Switching profile or session aborts in-flight
  avatar requests (abort + reset block, reference lines 183-192).
- Load states: `settings.profileLoading` line while the IAM GET runs; load
  error shows `settings.profileLoadError` with a `settings.profileRetry`
  text button; save errors map `settings.profileUpdateUnsupported` /
  `settings.profileForbidden` / `settings.profileSaveError`.

## Edit avatar dialog (`-edit-avatar-dialog.ui.tsx`)

Popup 323 px wide, radius 12, `bg-bg-elevated` with a `border-border-subtle`
hairline and no shadow; inner column `px-3 py-5 gap-5` (12/20 px padding,
20 px between the action block and Cancel). Action rows `h-8 rounded-lg
gap-3` with a 32x32 icon box (glyph 24): "Choose from gallery" (`images`,
tone `text-text-muted`) and "Delete current photo" (`delete`, tone and label
`call-red`, disabled when there is no avatar); rows `text-sm leading-5
text-text-primary` (danger rows `call-red`), disabled `opacity 0.5`. Error
line `text-xs text-danger`. Cancel `h-10 w-full rounded-lg bg-card-bg-active
px-4 text-sm font-medium text-accent`. The camera row stays hidden exactly
like the reference.

Accepted mime kinds png/jpeg/gif/webp; an empty file or a wrong kind shows
`settings.avatarInvalidFile`; an oversized file shows `settings.avatarTooLarge`
with the capability limit (25 MiB); a server capability block shows
`settings.avatarChangesDisabled`. The 64 px header avatar previews the picked
local file immediately; a remove request falls back to the initials/default
rendering (the default-avatar URN reset path; native shows the initials
fallback, which is what the default URN renders for an empty avatar). Server
failure reopens the dialog with `settings.avatarUpdateError` /
`settings.avatarRemoveError` (401/403 -> `settings.avatarChangesDisabled`,
400/409/413/415/422 -> `settings.avatarInvalidFile`, 404/405/501 ->
`settings.avatarUnsupported`).

## Behaviour contracts

- Share (both variants): copies `buildWorkspaceProfileShareLink(organization
  origin, userUuid)` - `{origin without trailing slashes}/#user/{uuid}` with
  a canonical-lowercase UUID and a valid realm origin (https; plain http is
  only accepted for the loopback hosts the reference dev build allows,
  `validation.ts:isValidRealmUrl`); the chip flips to `check` + `Copied` for
  2000 ms; the chip is disabled when no link can be built.
- Message (other users only, `-direct-navigation.lib.ts` + tests): already on
  the DM stream (any topic) -> stay; DM stream exists elsewhere -> open its
  default topic; DM stream missing -> create the direct stream and open the
  default topic (native reuses the verified `startDirectChat` lookup/create
  machinery from #110, which enforces the same default-topic validation);
  existing stream without a valid default topic logs an error and does
  nothing. The chip is disabled while a create is pending.
- Call: same navigation target with a pending call handoff; the Jitsi DM call
  itself is gated on epic #33, so native renders the Call chip disabled.
- Own-profile detection: `userUuid == verified session user`.
- Switching profile/user or signing out resets edit state and aborts avatar
  requests. All async projection updates are guarded by the owner key, the
  API generation and a per-projection ticket (the channel-info #132 review
  pattern), so a stale response can never apply after a session or profile
  change. Following the #197 lesson the QML page never fetches while hidden:
  the visibility binding gates the IAM load and the loads re-arm on
  `onVisibleChanged`.

## Native wiring

- New `packages/native/qml/NativeUserProfile.qml` mounted in the
  `NativeRightDrawer` body slot in `Main.qml`, visible when the top screen is
  the chat-info profile override (`currentProfileOverride ==
  "workspace-user-uuid:{uuid}"`) or the account `personal-info` screen (own
  profile). `rightDrawer.title` follows the page header title
  (`settings.personalInfo` for self, `info.information` for others).
- Entry points (the #72 surface): the account menu "Personal info" button
  calls `rightDrawer.openPersonalInfo()`; the direct-chat header
  `side_panel` affordance opens the partner profile override
  (`rightDrawer.openWorkspaceUserProfile(peer)`) for DM streams (ordinary
  channels keep the not-yet-implemented affordance); workspace mention links
  in messages route through the existing `openReference` hook to
  `openWorkspaceUserProfile`.
- `NativeController` gains the profile projection: `profileSnapshot(userUuid)`
  (name/avatar/status/email/joined from the verified directory),
  `profileDetails(userUuid)` (the reference detail list), `profileShareLink
  (userUuid)`, own-profile state `ownProfile`, `ownProfileBusy`,
  `ownProfileSaving`, `ownProfileError`, `ownProfileRefreshPending`,
  `ownProfileValid`, the invokables `loadOwnProfile`, `saveOwnProfile(patch)`,
  `uploadOwnProfileAvatar(path)`, `removeOwnProfileAvatar()`,
  `validateAvatarFile(path)` and `openDirectChatFromProfile(userUuid)` plus
  `profileDirectPending`. The pure resolver
  `workspace::resolveProfileDirectNavigation` lives in `domain.cpp` next to
  the DTO helpers so the controller test can drive every branch. Avatar
  upload is a new `ApiClient::startUserAvatarUpload` multipart transfer
  (form field `file`) mirroring `startFileUpload`'s credential scoping.
- New icons added to the pinned reference icon set: `stylus`, `images`,
  `border_color`, `links`, `business_center`, `handshake`, `account_circle`,
  `group`, `globe_location_pin`, `schedule`, `calendar_month`, `celebration`.
- i18n: every visible string above gets the exact reference en text and the
  exact reference ru text under the `NativeUserProfile` QML context and
  `workspace::NativeController` (locale catalog guard).

## Test plan

`native-native-window` (window fixture with the mock server):

1. Self profile ("Personal info" from the account menu): 64 px avatar, name,
   presence label colored via the presence text map; detail rows show userId,
   email and the formatted joined date, unsupported rows show the
   temporarily-unavailable label italic; the Favorites/Edit/Share chips render.
2. Other-user profile: Message/Call/Share chips render, Call disabled; Share
   copies `{origin}/#user/{uuid}` and the chip flips to "Copied" then reverts
   after ~2 s; Message on an existing DM stream selects its default topic,
   Message with no DM stream creates it (POST /streams/) and lands there.
3. Edit mode: Edit chip swaps the view; the display-name preview follows the
   first-name draft; Save issues the IAM PUT with only changed keys, updates
   the profile and exits edit mode; an invalid draft disables Save.
4. Avatar dialog: opens from the 20 px badge, is 323 px wide with 32 px rows
   and a 40 px Cancel; the remove row is disabled without an avatar; an
   oversized file shows the capability-sized message and a wrong mime shows
   the invalid-file message; a valid pick previews and Save uploads the file
   as multipart `file`.
5. Reset semantics: switching the profile or signing out resets edit/avatar
   state; hidden pages never issue profile GETs (visibility gate).

`native-native-controller`: pure `resolveProfileDirectNavigation` branches
(port of `right-panel-user-profile-direct-navigation.lib.test.ts`);
`profileDetails` value/format contract; share-link builder; own-profile
load/PUT (patch contains only changed keys, refresh-pending detection via the
directory re-read); avatar upload multipart + reset POST + error mapping;
`openDirectChatFromProfile` against the direct-chat machinery (open existing,
create missing, already-open no-op).

## Window test evidence (window-group round)

`native-native-window` implements the plan above in
`packages/native/tests/native-window-test.cpp` on a `ProfileWindowScene`
(`WindowProfileFixture` over the seeded direct-chat window fixture: the mock
serves GET|PUT `/api/core/v1/iam/users/{uuid}`, the messenger projection read
and the avatar_reset/avatar_upload actions):

- `windowProfileOwnScreenHeaderChipsAndDetails` - account menu -> Personal
  info: shell title, 64 px avatar, 8 px indicator-green presence dot
  (`#26c038`, tokens.ts), name, "online" presence line, Favorites/Edit/Share
  chips, userId/email/joined values, "Temporarily not connected" placeholder,
  Copyable only on value rows, IAM GET issued exactly once on mount.
- `windowProfileOtherUserMessageOpensDirectDefaultTopic` - "Information"
  title, Message/Call/Share chips, Call disabled at opacity 0.5 (gated on
  epic #33), Message selects the seeded DM stream + its default topic
  (OpenDefaultTopic) and re-clicking on the DM stream is the AlreadyOpen
  no-op.
- `windowProfileEditModeSavesDiffOnlyAndSyncsPreview` - footer 110x40 Cancel
  and filling 40 px Save, display-name preview tracks the first-name draft,
  Save PUTs `{"first_name": "Kassi"}` only (diff-only contract), the
  projection refresh clears refresh-pending, edit mode exits on settle and
  the header shows the saved name.
- `windowProfileAvatarDialogGeometryAndDisabledRemove` - 20 px badge opens
  the dialog at its fixed 323 px width; 32 px rows with the reference gap-2
  (rows start 40 px apart); 40 px Cancel; Delete disabled without an avatar;
  Choose opens the file dialog and Cancel closes the popup.
- `windowProfileAvatarFileValidationReopensDialogWithError` - a > 25 MiB
  file reopens the dialog with "Image is too large (max 25 MB)" and a wrong
  mime with "Please choose a valid image file", zero upload requests; the
  page stays in edit mode. The server-side upload/preview happy paths stay
  covered by `native-native-controller` (multipart contract).
- `windowProfileAvatarResetCommittedOnSave` - pending removal with a clean
  draft takes the empty-patch shortcut: no identity PUT, exactly one
  `avatar_reset/invoke`, edit mode exits on settle.
- `windowProfileShareCopiesLinkAndFlipsToCopied` - clipboard equals
  `{origin}/#user/{uuid}`, the chip reads "Copied" and reverts to "Share"
  after the 2000 ms copyable flip-back.

Found and fixed while wiring these cases (both reproduced RED first):

- The action-chip `Repeater` delegate instantiated the `ProfileChip`
  component with `required property var modelData` on the component root;
  under `pragma ComponentBehavior: Bound` the view context injection cannot
  satisfy a required property owned by a separate component root, so every
  chip delegate failed at runtime ("Required property modelData was not
  initialized"). The delegate is now an inline root that feeds the chip
  explicitly, same pattern as the detail rows.
- The page's `Connections` reopened the avatar dialog on
  `ownAvatarErrorChanged`, a signal the controller never had, so failed
  mutations surfaced no error. `NativeController` now declares
  `ownAvatarErrorChanged()` and emits it on every mutation of
  `m_ownAvatarError` (the property keeps `stateChanged` as NOTIFY for value
  bindings).
- The avatar dialog rendered the two action rows at the outer 20 px rhythm;
  the reference groups them in a `gap-2` (8 px) block before the 20 px gap
  to Cancel. The row block now matches, which also makes the derived dialog
  height the measured 172 px stack instead of 184 px.
