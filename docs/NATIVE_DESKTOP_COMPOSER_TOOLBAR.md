# Native composer formatting toolbar (issue #122)

Ports the pinned reference formatting toolbar to the native composer. Reference
contract: `workspace_ui` at `61dacef78dab2f5d0be76f283bebf49fe29b84af` under
`packages/web/src/widgets/message-composer/`. All measurements below were read
from that tree with `git show <sha>:<path>`.

## Toolbar shell (message-composer-styles.lib.ts, message-composer.ui.tsx:411-456)

- Toolbar row: `h-8` (32px) high, `flex-shrink-0`, rendered only when the
  persisted expansion flag is true (`if (!expanded) return null;`), placed as
  the bottom row of the composer column; the input row keeps the send control.
- Leading inset: `COMPOSER_LEADING_CONTROLS_INSET = "pl-2"` (8px) on the row's
  inner container; the compact expand toggle uses the same inset so both
  32px toggles sit on one vertical axis (measured assertion in the window test).
- Send clearance: `COMPOSER_TOOLBAR_SEND_CLEARANCE = "mr-[52px]"` - the send
  button is `h-10` (40px) plus a 12px row gap; the toolbar's right edge stops
  52px before the composer right edge (asserted as `send.right - toolbar.right == 52`).
- Every action button: `TOOLBAR_BTN = "flex h-8 w-8"` -> 32x32 hit area
  (asserted for every toolbar button).
- Separators between action groups: `span class="h-7 w-px bg-border-subtle"`
  -> 28x1 Rectangle tinted with the `border-subtle` palette token.
- Text-style cluster (link/B/I/S) sits flush: `TOOLBAR_TEXT_STYLE_GROUP` gap-0;
  inter-group `gap-2` (8px).
- Toggle placement: expanded toolbar leads with the collapse toggle
  (`aria-expanded`, label `composer.collapseToolbar`, `disabled={mode === "preview"}`);
  collapsed state keeps a single expand toggle in the input row
  (`compactInline` placement, `message-composer.ui.tsx:1925-1939`). The native
  client has no Write/Preview mode tabs yet (sibling issue), so the
  preview-mode disable rule is recorded but not native-testable today.

The composer shell reserves the toolbar's preferred height and the column gap
while the row is visible. Without that space, an empty or one-line draft left
the shell 57px high and placed its 32px controls at y=896..928 in a 1440x900
window. The collapsed input height and the separate edit/reply notice additions
keep their existing calculations.

`expandedComposerToolbarStaysInsideTheWindow` renders empty and short drafts in
both locales and themes, requires every complete button to fit inside the shell
and window before pointer input, and checks typing/deleting a multiline draft,
collapse/expansion and window resizing. At the 700×600 minimum window size the expanded row scrolls horizontally.
Moving keyboard focus reveals the whole focused action. Its real Bold click also checks draft
mutation, restored editor focus/caret and absence of message sends. These are
local Linux Qt Quick fixture checks, not live-service or other-platform acceptance.

## Actions and exact insert text (message-composer-toolbar.ui.tsx:40-320)

The AI control is visible while composing a new message or reply. Editing an
existing message hides the control.

Order in the expanded toolbar: toggle, AI, separator, [link, bold, italic,
strikethrough] (flush group), separator, [numbered list, bulleted list],
separator, [quote, code block, inline code, spoiler]. File/emoji/time/schedule
triggers are out of native scope today; the native groups follow the same
separator layout with the available actions.

- wrap `marker` (`message-composer-selection.lib.ts:32-51`): empty selection
  inserts `marker + marker` with the caret between (offset `marker.length`);
  a selection becomes `marker + sel + marker` and collapses the caret after the
  closing marker (offset `marker.length + sel.length + marker.length`).
  Markers: bold `**`, italic `*`, inline code `` ` ``, strikethrough `~~`,
  spoiler `||`.
- quote: empty -> `"> "` caret 2; selection -> each selected line prefixed
  `"> "`, caret after the replaced block.
- bulleted list: empty -> `"- "` caret 2; selection -> `"- "` per line.
- numbered list: empty -> `"1. "` caret 3; selection -> `"{i+1}. "` per line.
- code block: empty -> `` "```\n\n```" `` caret 4; selection ->
  `` "```" + "\n" + sel + "\n" + "```" `` caret after.
- link: selection `sel` -> `[sel](https://)` with the URL part
  (`https://`, without the closing paren) selected
  (`selectionStartOffset = indexOf("https://")`, end = `length - 1`). Empty ->
  the localized template `[<composer.linkText>](https://)` with the URL part
  selected. Native pins `selectionStart` to the fixed URL tail of the template
  (`length - 1 - "https://".length`) instead of a raw `indexOf` scan, so a
  label that itself contains `https://` cannot move the selection into the
  label (exo-review finding on PR #173; regression row `link-label-contains-url`).
- All mutations run through the existing native route/focus guard pattern
  (`wrapFormatting`: draft rebind, owner/stream/topic identity, scroll restore)
  extended to selection restoration for the whole action set.

## Figma glyph bounds (message-composer-icons.ui.tsx:19-116 and the toolbar test)

bottom-panel open/close 21.333x21.333, add-link 24x14.769, bold 10.93x16.667,
italic 15.448x16.747, strikethrough 25.333x19.79, numbered list 21.333x24,
bulleted list 21.328x19.533, quote (chat-dashed) 24x21.38, code block
(frame-source) 21.333x21.333, inline code 22x18, spoiler eye 25.834x17.333
with a `h-px w-7 rotate-[-35deg]` slash overlay. Native buttons use the same
assets recolored through `icon.color`, with glyph boxes rounded to whole
pixels (`glyphWidth`/`glyphHeight` are ints); the 32x32 hit area, separators
and clearance are the pixel-exact assertions.

## List continuation on Shift+Enter (message-composer-list-continuation.lib.ts:40-85)

Only when the caret (no selection) is at the end of a line:
- `^(\s*)([-*+])\s(.*)$` -> insert `"\n" + indent + marker + " "`; ordered
  `^(\s*)(\d+)\.\s(.*)$` -> insert `"\n" + indent + (n+1) + ". "`.
- If the matched line has blank content, the marker line is deleted
  (`text[:lineStart] + text[lineEnd:]`, caret `lineStart`) instead of extended.
  `replaceLineWithEmpty` slices `[lineStart, lineEnd)`, so the newline that
  terminates the preceding line is retained (exiting a trailing empty marker
  of "a\nb\n- " yields "a\nb\n"); the native implementation mirrors this
  exactly and the window test asserts it.
Native binds this to Shift+Enter inside the composer key handler (plain Enter
sends, so newline insertion is Shift+Enter there).

## Persistence (settings.model.ts at the same sha)

`composerToolbarExpanded` defaults to `false` and is stored with the global
layout settings (`parsed.composerToolbarExpanded === true`). The native
`NativePreferences` mirrors the sidebar-width pattern: key
`layout/composerToolbarExpanded`, global (not owner-scoped), read at load,
written in `persist()`, `composerToolbarExpandedChanged` signal; the QML binds
both composer instances to it so expansion survives restarts and composer
recreation.

## i18n (packages/web/src/i18n/locales/{en,ru}.json)

`composer.bold/italic/strikethrough/code/codeBlock/quote/link/linkText/spoiler/
numberedList/bulletedList` and `expandToolbar` "Show formatting tools" /
`collapseToolbar` "Hide formatting tools". Russian: Жирный, Курсив,
Зачёркнутый, Код, Блок кода, Цитата, Ссылка, текст ссылки, Спойлер,
Нумерованный список, Маркированный список, Показать/Скрыть инструменты
форматирования. Every new native `qsTr` string is added to
`packages/native/translations/workspace_ru.ts` (locale guard test).

## Hotkeys

Ctrl+B/Ctrl+I/Ctrl+E/Ctrl+Shift+X keep wrapping via the shared mutation path.
The reference binds no hotkeys to the other insert actions, so none are added.

When the toolbar is expanded, Tab from the composer reaches Link, followed by
the other formatting actions in their displayed order. Shift+Tab from Link
returns to the composer. Keyboard focus uses the existing button focus ring,
and the text selection stays visible while an action has focus. Return, keypad
Enter and Space use the same formatting path as clicking, then return focus to
the composer with the action's selection/caret result.

`composerToolbarKeyboardNavigationPreservesSelection` exercises real keyboard
selection, forward/reverse focus navigation and all ten actions for each
activation key in English/Russian and light/dark themes. It checks the retained
draft, final caret/URL selection and absence of message sending. Existing mouse
action and shortcut tests cover the same mutation helpers.
The keyboard case covers both short and three-line drafts and requires each
action to be inside the window, including the layout repair from #314. It also
checks traversal beyond the last action and skips formatting actions when the
toolbar is collapsed.

The expansion persistence test measures the three 1 × 28 separators and the action positions after the expanded toolbar presents a frame. The visibility notification arrives before the layout has assigned the rectangles their size.

## AI availability notice

The pinned web toolbar places a 32×32 AI assistant button immediately before
the separator that precedes Link. Collapsing the toolbar hides the button.
The button displays “AI” at 20 px and exposes its pressed state to assistive
technology. Activating it opens the organization's localized availability
notice. A second activation, Escape, or a click outside dismisses the notice. The transparent backdrop consumes that
click, including when Send is underneath it.
The notice and the existing composer pickers are mutually exclusive.

The web composer reserves an 88 px positioning box above the anchor, with
an 8 px gap, and centers the panel horizontally within the viewport. The panel
itself fits its 12 px text and 12 px padding. In the deployed desktop web
composer the English notice measured 320×58 px. The reference uses the
accessible name “AI assistant” in both languages and translates the notice
through `composer.aiTemporarilyUnavailable`. The four window test rows check
the actual button, Escape and pointer dismissal, the return to keyboard focus,
and the file and emoji pickers with an unchanged draft. Native scheduling
and saved-snippet controls have no entry point in this composer yet.
