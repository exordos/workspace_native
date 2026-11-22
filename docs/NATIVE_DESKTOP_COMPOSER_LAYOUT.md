# Composer action placement parity (issue #661)

Reference contract: `workspace_ui` @ e0194ffb667986b58a12256ce41ca5dc99b56b83,
`packages/web/src/widgets/message-composer/`. This doc completes the trigger
placement contract opened by `NATIVE_DESKTOP_POLL_CREATION.md` ("the poll
trigger sits after the emoji trigger in the toolbar order: file/call ->
emoji -> poll") and supersedes the `NATIVE_DESKTOP_COMPOSER_TOOLBAR.md`
scoping note that kept the file/emoji/poll triggers out of the toolbar.

## Reference layout model

The composer has two shapes, keyed by the persisted
`composerToolbarExpanded` setting (`settings.model.ts:174`, parsed
strictly, default `false`; mirrored by `NativePreferences`,
`src/native-preferences.cpp:149`):

- Collapsed (default). The expanded toolbar renders nothing
  (`message-composer.ui.tsx:443` `if (!expanded) return null;`). Inside the
  input box the reference renders:
  - leading: the expand toggle only
    (`MessageComposerCompactLeadingControls`, `message-composer.ui.tsx:206-238`,
    `data-testid="composer-compact-controls"`);
  - trailing actions (`compactSecondaryActions`, `message-composer.ui.tsx:1949-1960`):
    `MessageComposerCompactSecondaryActionControls` = ATTACH + EMOJI ONLY.
    There is no poll trigger in the collapsed composer anywhere; in the
    vertical rail (tall editor) the rail actions are the same attach + emoji
    set (`message-composer.ui.tsx:2171-2175`).
- Expanded. One row under the input (`ScrollArea h-8`,
  `data-testid="composer-toolbar-row"`): toolbar toggle, Write/Preview mode
  tabs, then the formatting toolbar (`message-composer-toolbar.ui.tsx:145-159`,
  `role="toolbar"`, flex `gap-2`, `pl-2` leading inset):
  `fileTrigger, callLinkTrigger, [h-7 w-px bg-border-subtle separator],
  emojiTrigger, pollTrigger, time-insert, scheduleTrigger, snippetsTrigger,
  aiTrigger, |, [link bold italic strikethrough], |, [numbered bulleted],
  |, [quote code-block inline-code spoiler]`.
  The poll trigger is the 24px `PollButton`
  (`message-composer.ui.tsx:131-141`, title `pollForm.title`) and exists ONLY
  in this row.

## Native mapping

- `composerCompactActions` (the in-field grid in `Main.qml`) is now visible
  only while the toolbar is collapsed (was: always outside preview), and keeps
  exactly attach (col 0) + emoji (col 1) - the reference `compactSecondaryActions`
  set. The poll trigger is removed from the grid.
- `composerToolbarActions` (the expanded `GridLayout`, `columns: 19`) gains,
  at the reference positions: attach (column 2), the file/emoji separator (3),
  emoji (4), poll (5); the pre-existing time/snippets/AI and format groups
  shift to columns 6..18 with explicit `Layout.column` values so a reparented
  child cannot land at the row tail (`GridLayout` honours the `Layout.column`
  attached property).
- Attach and emoji are single instances reparented declaratively: expanded ->
  `composerToolbarActions`; collapsed -> the in-field grid, or the tall-editor
  left rail (`composerLeadingControls`, the documented addition of
  `NATIVE_DESKTOP_COMPOSER_HEIGHT.md`, mirroring the reference vertical rail
  which carries the same two actions). Their `Layout.column` follows the
  active layout so both arrangements are positional, not insertion-order.
- The poll trigger lives only in `composerToolbarActions`; while the toolbar
  is collapsed it is absent from the window, matching the reference exactly.
  The poll dialog itself (`NATIVE_DESKTOP_POLL_CREATION.md`) is unchanged.

## Deviations (named)

- The toolbar leads with collapse + preview toggles where the reference has
  toggle + Write/Preview mode tabs: the native equivalents shipped in #122
  (`NATIVE_DESKTOP_COMPOSER_TOOLBAR.md`) and keep their leading position.
- callLink and schedule triggers do not exist natively (no native call-link /
  scheduling composer surface yet); the reference omits an absent trigger and
  native follows. The file/call separator therefore keys on the file trigger.

## Acceptance checks (`native-window-test`)

- Collapsed default: `pollButton` is NOT visible anywhere; `attachButton`
  and `composerEmojiButton` render in the field to the right of the input
  (existing compact geometry contract, shell height 57 row of #122 unchanged).
- Expanded (click `composerToolbarExpand`): `attachButton`,
  `composerEmojiButton`, `pollButton` render inside `composerToolbarRow`
  below the input viewport, strictly ordered left to right
  attach < emoji < poll < time; the poll trigger keeps the 32px hit area and
  the `pollForm` title contract from #215.
- All existing poll window cases create a poll through the expanded toolbar
  (helper expands first), and the #122 toolbar containment case stays green.
