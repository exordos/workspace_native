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
- Keyboard parity (round 5): the reference chip is a real `<button>`, so it
  is part of the browser tab order. The native overlay chip sets
  `activeFocusOnTab: true` and therefore joins the window tab chain: Tab /
  Shift+Tab reach it without any pointer interaction, Enter and Space
  activate the same `openReference` path, and a visible focus ring marks the
  focused chip. An earlier revision kept `activeFocusOnTab: false` to avoid
  perturbing the pinned focus orders; review thread `discussion_r4639284120`
  rejected that (a keyboard-only user could then never reach the chip), and
  the reaction focus-order cases pin no mention chips, so the tab stop is
  the correct deviation point.

In an editable topic the bubble's first Tab stop is Reply. Subsequent stops
include the mention links in message order. While a mention has focus, Reply
stays available to Shift+Tab. Channel-wide messages expose their mentions
directly. Enter and Space open the focused user's profile.

## Tests (native-native-window, `tests/native-window-test.cpp`)

- A body containing a mention urn renders a visible
  `message-mention-<urn-suffix>` item over the body text; a silent mention
  renders its chip without the `@` prefix in the painted text.
- A plain-text mention with no uuid (`@Ghost`) renders unstyled: the body text
  contains `@Ghost` and no chip item exists for it.
- Clicking the chip raises the profile-open path: the workspace profile layer
  (`profileLayer`) opens for the mentioned uuid.
- Keyboard-only reach (`mentionChipsReachedByTabWithoutPointer`,
  `mentionChipsSpaceActivationFromTabFocus`): driving the window with Tab and
  Shift+Tab only (no QTest mouse event, no `forceActiveFocus`), the focus
  chain lands on the chip, Shift+Tab leaves it, Tab returns, and Enter
  respectively Space then opens `profileLayer`.

## Live verification

Verdict: PASS (2026-11-20, display :31, reference sha 61dacef78dab2f5d0be76f283bebf49fe29b84af).

- `live_qa.py native-login --display :31 --bin build/lane/workspace-native`
  succeeded; `~/dev/qa/evidence/109/stage4_workspace.png` and
  `~/dev/qa/evidence/109-rerun/stage4_workspace.png` show the sidebar and
  stream list after sign-in (vision-checked, no "Sign-in was rejected").
- Mention send through the service path:
  `live_qa.py send --stream-uuid 59033cd5-64ba-4a00-a932-5dddb5c17b65`
  with text `CASSI QA 2026-11-20T06:03:13Z [Alice QA](urn:user:2f2a2a2a-2a2a-4a2a-8a2a-2a2a2a2a2a2a) plain @Ghost mention`:
  message_uuid b3477270-85cc-58e5-9a92-1054c8b755da, send_ms 120.3
  (< 1500), visible_in_list_ms 47.5 (< 3000), read back OK.
- Rendered chip screenshot (final binary):
  `~/dev/qa/evidence/109/step-mention-final.png` (native qa room) -
  vision check: "@Alice QA" paints in the accent token colour with no
  underline; "plain @Ghost mention" is unstyled plain text (the uuid-less
  degradation). An earlier `step-mention.png` captured an intermediate build
  before the underline removal.
- `live_qa.py health --repeat 10`: p50 7.8 ms, p95 13.7 ms (< 500 ms).
- Plain-text send `CASSI QA <UTC>`: send_ms 69.9, visible_in_list_ms 41.6,
  read back OK.
- Cross-user check: SKIPPED (no second QA account in the vault).

## Out of scope

- Profile cards themselves (#4/#72); until they land the drawer's existing
  content/coming-soon notice is the accepted click outcome.
- Quote previews reuse the same renderer but their HTML is styled by the
  quote frame; mention chips there keep the existing generic link handling.
- Hover opacity (the reference hover is 0.9 opacity on the button; native
  anchors do not implement per-run hover - visually negligible, documented
  deviation).
