# Message timestamp viewer (issue #108)

Part of #105. Parity-preserving reproduction of the read-only message time
viewer from the packaged web reference
(`workspace_ui` @ `61dacef78dab2f5d0be76f283bebf49fe29b84af`).

## Reference consultation notes

Figma token unavailable - mockups not consulted (the `figma-token` helper
returned an empty value at implementation time). The reference source tree at
the contract sha is the only oracle; every number below is measured there.

Electron source: n/a (the feature lives in the web bundle only).

### Reference panel: `workspace-message-time-view.ui.tsx`

Measured at the contract sha (all citations `packages/web/src/widgets/
workspace-message-list/workspace-message-time-view.ui.tsx`):

- `PANEL_WIDTH = 371` (`:9`), `PANEL_FALLBACK_HEIGHT = 490` (`:10`).
- The panel is an `AnchoredPopover` whose panel style adds
  `maxHeight: "calc(100dvh - 16px)"` and `overflowY: "auto"` (`:67`), so the
  panel never exceeds the viewport minus 16 px (8 px margin on both sides).
- The panel receives focus on open (`panel.focus({ preventScroll: true })`,
  `:42`); the popover repositions on anchor/viewport changes via
  `ResizeObserver` + `resize`/`scroll` listeners (`:40-55`).
- The instant is parsed once (`new Date(utc)`, `:57`); an unparseable value
  renders nothing (`:58`).
- Content is a `DateTimePicker` in read-only mode: `readOnly` (`:72`),
  `onValueChange` is a no-op (`:74`), and seconds are shown only when the
  stored instant carries a non-zero seconds or milliseconds component:
  `showSeconds={date.getSeconds() !== 0 || date.getMilliseconds() !== 0}`
  (`:75`). The picker itself renders with a `!w-full` className and squared
  date/time sub-panels (`:76-78`).
- Closing (`onClose`) also returns focus to the triggering time element
  (`anchor.focus(...)`, `:63-66`).
- Reference behaviour tests (`workspace-message-time-view.ui.test.tsx`):
  the dialog is named "Date and time", the calendar grid reports
  `aria-readonly="true"`, a "Time" group exists, there are zero buttons and
  zero spinbuttons inside the panel (`:19-20`), exactly one selected grid
  cell (`:21`), focus is on the `tabindex="-1"` panel wrapper (`:22`), the
  time `status` shows `HH:mm` (`:28`); Escape dismisses exactly once and
  refocuses the anchor without touching the stored value (`:31-46`).

Panel shell (`packages/web/src/shared/ui/anchored-popover/anchored-popover.ui.tsx`):
`rounded-xl border border-border-subtle bg-bg-elevated shadow-xl` (`:5-6`),
`role="dialog"` with the `dateTimePicker.label` accessible name (`:39-40`),
outside click and Escape both dismiss (`:20-24`, `:34`).
`dateTimePicker.label` is "Date and time" (en) / "Дата и время" (ru)
(`packages/web/src/i18n/locales/en.json:20`, `ru.json:20` at the sha).

Floating placement (`packages/web/src/shared/lib/floating-panel-position.lib.ts`):
viewport margin 8 px (`:1`), anchor gap 8 px (`:2`); the panel is centered on
the anchor horizontally and clamped into the viewport (`:28-30`); it prefers
sitting above the anchor with the 8 px gap (`:32-34`) and otherwise falls
below the anchor, clamped (`:37-45`).

Read-only picker geometry (`packages/web/src/shared/ui/date-time-picker/date-time-picker.ui.tsx`):
container `role="group"` with `aria-label` `dateTimePicker.label` and
`w-[371px]` (`:40-46`), a `DatePicker` calendar (`:48-61`) and a `TimePicker`
(`:62-87`) that shows the local `HH:mm[:ss]` rendering of the stored instant
(`localTime(selected, showSeconds)`, `:65`; `date-time-picker.lib.ts:17`).
Date and time are displayed in the viewer's local timezone, exactly like the
chip that triggered the panel.

### Reference trigger: message time chip

`packages/web/src/widgets/workspace-message-list/workspace-message-body-interactions.hook.ts`:

- The trigger is the message `time` element carrying a non-empty `[datetime]`
  attribute: `closest("time.workspace-message-time[datetime]")`, and the
  stored UTC string is read from that attribute (`:72-81`).
- Click: resolving the time target and opening the viewer with
  `preventDefault`/`stopPropagation` (`:566-572`).
- Keyboard: `Enter`/`Space` on the focused time element opens the same
  viewer, also with `preventDefault`/`stopPropagation` (`:490-496`).

States to reproduce: light + dark themes, `ru` + `en` locales (locale-
formatted dates inside the panel).

## Native counterpart (measured on master `e5aff2c`)

- `packages/native/qml/Main.qml:8681-8693`: `messageTime` is a static `Text`
  (`objectName: "message-time-" + message.id`, right-aligned in the bubble,
  `font.pixelSize: 12`, `height: 16`, `text-muted`) showing `modelData.time`.
- `packages/native/src/native-controller.cpp:14276-14277`: every message row
  already carries the exact stored instant as `createdAt`
  (`message.created.toUTC().toString(Qt::ISODateWithMs)`, i.e.
  `YYYY-MM-DDTHH:mm:ss.zzzZ`) - this is the native `[datetime]`.
- `packages/native/src/workspace-reference.cpp:328-333`: the `urn:time:`
  reference kind parses through `parseWorkspaceMessageTime` (`:200-229`,
  kind name `"time"` at `:159`) and exposes the normalized instant under the
  `utc` map key (`:191`), but `openReference` (`Main.qml:1400-1429`) never
  handles it, so such links fall through to the "under development" notice.

## Native design

- Chip: the `messageTime` text becomes an interactive read trigger (pointer
  click + focusable with `Enter`/`Space`, matching the reference click
  `:566-572` and keyboard `:490-496` contract). Firing opens the panel with
  the row's `createdAt` and returns focus to the chip on close, mirroring
  `:63-66`.
- `openReference` gains a `time` branch: `reference.kind === "time"` opens
  the same panel with `reference.utc` (the parsed/normalized instant),
  replacing the dead fall-through to `referenceNotice`.
- Panel: a non-modal `Popup` named `messageTimePanel`:
  - width 371 px (`PANEL_WIDTH` :9), fallback height like the reference
    490 px measured content, maximum height `window.height - 16` px
    (parity of `calc(100dvh - 16px)` :67);
  - placement mirrors `computeFloatingPickerPosition`: 8 px viewport margin,
    8 px anchor gap, centered on the trigger, prefer above, clamp into the
    window (`floating-panel-position.lib.ts:1-2,28-45`);
  - shell palette `bg-elevated` + `border-subtle`, radius 12 (Tailwind
    `rounded-xl`), matching the `AnchoredPopover` class (`:5-6`) and the
    native popup convention (`streamMenu` background, `Main.qml:11033-11036`);
  - content is fully read-only: a locale-formatted month/year caption, a
    seven-column day grid with the stored day selected and no interactive
    controls (no buttons/spinbuttons, parity of the reference test
    `:19-21`), and a status-style time text
    `HH:mm` (or `HH:mm:ss` only when the stored seconds or ms are non-zero,
    parity of `:75`), all rendered in the local timezone like the reference
    `localTime`.
  - dismiss: `Escape` or any click outside (parity `anchored-popover.ui.tsx
    :20-24,34`); focus returns to the triggering chip (`:63-66`).
- Accessible/visible label: "Date and time" / "Дата и время"
  (`dateTimePicker.label`), added to `workspace_ru.ts` for the locale guard.

Design cross-check deltas (recorded for review):
- Tailwind `shadow-xl` has no 1:1 Qt equivalent; the native popup uses the
  established border + elevation convention instead (same as `streamMenu`).
- Calendar caption/format strings come from `QLocale` (ru month names in
  nominative case) instead of `date-fns` (genitive "января"); geometry and
  behaviour stay parity, the ru month name form is an accepted delta.
- The reference panel hosts the full editable DateTimePicker with all cells
  focusable; the native read-only panel renders the same grid as inert text
  cells because nothing in a read-only view is operable (reference test
  pins zero buttons/spinbuttons).

## Acceptance mapping (native-native-window)

- Clicking the `message-time-<id>` chip opens `messageTimePanel` at width 371
  showing the stored instant to the second only when seconds/ms are non-zero
  (`:75`); the minute-exact message shows `HH:mm` only.
- The panel is read-only (no button/spinbutton descendants), shows the
  selected day cell, and Escape closes it without mutating anything.
- Keyboard trigger: focusing the chip and pressing `Enter`/`Space` opens the
  same panel (parity `:490-496`).
- `openReference("urn:time:...")` opens the panel with the normalized
  instant instead of the fallback notice.
