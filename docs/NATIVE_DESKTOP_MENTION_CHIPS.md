# Native message mention chips (#109)

Figma token unavailable - mockups not consulted. The oracle is the reference
contract block in issue #109 (workspace_ui
`61dacef78dab2f5d0be76f283bebf49fe29b84af`).

## Reference behaviour (measured from source)

- `workspace-message-render.lib.ts:342-352` (`renderMentionToken`): a mention
  token renders as a `<button class="workspace-message-mention">` carrying the
  `@` prefix. Silent mentions render without the prefix. A mention whose
  `userUuid` is missing/blank degrades to escaped plain text (still with the
  `@` prefix for non-silent tokens).
- `workspace-message-body-interactions.hook.ts:611-624`: an unmodified primary
  click on `[data-workspace-mention=true][data-workspace-user-uuid]` always
  `preventDefault()`s and calls `onOpenMentionUser(uuid)`; the keyboard path
  (`:490-496`) activates the same callback from Enter/Space.
- Presentation classes (`messenger-workspace-message-body.ui.tsx:143-153`):
  the chip is inline text with **no background, border or padding** -
  `font-medium` weight, `text-accent` colour, pointer cursor, opacity 0.9 on
  hover. So "chip" here means accent-styled inline text, not a pill.
- Wire form: `[Label](urn:user:<uuid>)`, with the link title `silent` marking
  silent mentions (native composer already emits this form,
  `src/workspace-reference.cpp:383`).

## States

Light and dark themes take the chip colour from the palette token `accent`
(`root.colors.accent`; light default `#7087ff`), so both themes are covered
by the token. ru/en need no new user-visible strings (the `@` prefix and the
label are data, not UI text). Silent mentions and uuid-less degradation follow
the reference rules above.

## Native design

### Renderer (`src/native-markdown.cpp`)

`NativeMarkdown::render()` post-processes the md4c HTML before the
`QTextDocument` import:

- Every `<a href="urn:user:<uuid>">label</a>` (optionally `title="silent"`)
  becomes an anchor whose label gains the `@` prefix unless it is silent (a
  label that already begins with `@` is not doubled); the `title` attribute is
  dropped because the Qt importer discards it anyway.
- The rendered result map gains `mentions`: the ordered list of
  `urn:user:<uuid>` occurrences in the body, used by the view to place hit
  targets.
- Plain-text `@name` tokens (no urn) are untouched by the renderer and
  therefore render as unstyled text, matching the reference degradation for
  unresolved mentions. Invalid `urn:user:<not-a-uuid>` anchors keep the
  existing behaviour of collapsing to plain text.

### Bubble view (`qml/Main.qml`)

- `bodyHtml` and image-part HTML rewrite each mention anchor's exported span
  style to `color: <accent token>; font-weight: 500` (no underline) so the
  chips follow the active light/dark palette token.
- Each message bubble creates one chip item per mention occurrence with
  `objectName: "message-mention-" + <urn suffix>`, where the suffix is the urn
  with the `urn:user:` prefix removed (the user uuid). Qt Quick `Text` has no
  per-anchor item (inline components were removed in Qt 6), so the chip is an
  invisible positioned overlay: its geometry is located by scanning the
  visible content Text's `linkAt(x, y)` map for the mention urn (only for
  bodies that actually contain mentions; results are cached per
  width/height/urn-signature). The overlay supplies the pointer cursor, the
  `Accessible.role: Button` semantics and the click handling
  (`root.openReference(urn)`), while the visible painting stays the rich-text
  anchor itself - pixel-identical to the reference inline button style.
- The click keeps the established profile-open path: `openReference` already
  routes `urn:user` to `rightDrawer.openWorkspaceUserProfile(uuid)` (#134).

## Tests (native-native-window, `tests/native-window-test.cpp`)

- A body containing a mention urn renders a visible
  `message-mention-<urn-suffix>` item over the body text; a silent mention
  renders its chip without the `@` prefix in the painted text.
- A plain-text mention with no uuid (`@Ghost`) renders unstyled: the body text
  contains `@Ghost` and no chip item exists for it.
- Clicking the chip raises the profile-open path: the workspace profile layer
  (`profileLayer`) opens for the mentioned uuid.

## Out of scope

- Profile cards themselves (#4/#72); until they land the drawer's existing
  content/coming-soon notice is the accepted click outcome.
- Quote previews reuse the same renderer but their HTML is styled by the
  quote frame; mention chips there keep the existing generic link handling.
- Hover opacity (the reference hover is 0.9 opacity on the button; native
  anchors do not implement per-run hover - visually negligible, documented
  deviation).
