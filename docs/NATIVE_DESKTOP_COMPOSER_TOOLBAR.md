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

## Actions and exact insert text (message-composer-toolbar.ui.tsx:40-320)

Order in the expanded toolbar: toggle, separator, [link, bold, italic,
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
  selected.
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
