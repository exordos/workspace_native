# Existing attachments in an edit session

Issue #189, parity-preserving design. Reference: workspace_ui
`61dacef78dab2f5d0be76f283bebf49fe29b84af`,
`workspace-composer-edit-attachments.lib.ts` and
`message-composer-controlled-attachments.ui.tsx`.

Starting an edit extracts leading and trailing attachment-only lines into
removable cards. The pinned implementation keeps inline image markdown in the
editable body while also listing those images as cards; this differs from the
issue's prose. Removing an inline image card removes its actual image reference,
without deleting surrounding text or matching examples inside code. Save
prepends retained verbatim attachment lines once, preserving inline positions
and avoiding duplicates with newly uploaded references.

Parsing uses the existing MD4C Markdown parser and Workspace reference validator.
Code, external links, quote references and malformed file references stay text.
The source-range scanner supports single-line direct links/images and full,
collapsed or shortcut reference-style images resolved by MD4C. Reference-style
images keep their body and definitions intact, matching the pinned extractor.
Multiline/nested source labels remain editable Markdown. No network request is
needed to extract metadata. The controller owns attachment inventory inside the current
edit session, including separate inventories for restored reply answers. Removal
advances the existing edit revision so an older save cannot close a newer edit.
Cancel, route/owner loss and reopening reset this inventory. Existing upload
session and in-flight save guards remain authoritative.

The composer lists 240 by 58 pixel cards (bounded by available width), 8 pixel
padding/radius, 40 pixel previews, 12 pixel filename and metadata, and a 24 pixel
Remove action. A horizontal strip keeps the input reachable for many cards.
Image cards reuse the authenticated native image manager through
NativeMessageImage; file icons remain visible while a preview loads or fails.
The image lease ends when the card/session disappears. English/Russian and
light/dark use existing localization and palette mechanisms.

Validation must first fail on the unchanged product, then cover extraction,
verbatim save/removal, attachment-only edits, code/duplicate handling, reply-tab
and in-flight lifecycle, and the real visible composer in both locales/themes,
including held and failed preview requests. Local synthetic HTTP/window results
do not establish live-service, platform picker or Windows/macOS acceptance.

## Editing retained and inline references

Extracted boundary lines are retained attachments; inline cards describe parsed
references still owned by the editable text. Save prepends only retained lines,
and suppresses a duplicate only when the current body contains the actual parsed
attachment. Matching code examples or escaped markup do not replace an attachment.
Typing resynchronizes inline cards from the current parsed body, including changed
reference definitions. An unchanged card keeps its session ID; a changed target
gets a new ID so the old image lease cannot remain associated with it. Retained
cards survive text edits and remain scoped to the same reply answer. This repairs
the two reproduced review findings; the pinned source has the same substring and
stale-inventory behavior, which is not preserved when it loses user edits.

## Local acceptance

The unchanged controller reproduced the defect with valid leading file/image
references: the edit buffer still contained both URN lines instead of the
caption. The regression now observes the extracted inventory and editable body.
A second regression first confirmed that a reference-style image already
rendered by the native message body was missing from the edit inventory; the
pinned extractor also lists that image. Controller checks cover its removal and
save, three restored references, a real held file upload,
verbatim save without duplicates, inline removal that preserves code examples,
attachment-only editing, stale card IDs, navigation/cancel, retained reply-tab
inventories and removal while an earlier PUT is pending.

Visible Qt 6.10.2/XCB checks use the real Main.qml, Session, FileClient and
MessageImages against a local synthetic HTTP server. All eight combinations of
English/Russian, light/dark and successful/failed image previews exercise the
held loading state, Remove by keyboard, typed inline removal, Save and session
teardown. Each preview/locale/theme combination runs with both removal paths. Card geometry
and dark fallback icon pixels are checked. The initial candidate painted SVG
currentColor black on the dark card; that paint regression failed before the
icon was routed through the existing palette-aware native icon control.
The controller suite has 61 passing inline-edit cases, with 65 Markdown and 14
locale cases. Before the review fix, six real outgoing PUT regressions lost
retained attachments or restored deleted/changed inline references, and a seventh
check found a stale card after changing its reference definition. Separate
coverage preserves the retained role when a leading and trailing reference are
identical, and unchanged card IDs across reply-tab edits. Captures use only
synthetic data and are kept outside the product tree.

All 16 card scenarios passed in both combined visible runs. Each combined run
finished with 23 passes and one failure in the existing upload-picker teardown,
after its functional assertions passed. The picker passed alone and in a later
two-scenario run without product changes. A temporary diagnostic confirmed that
its frame and incubation acknowledgement passed, while the common hide/focus/close
guard failed. No general teardown change is included; the combined-run limitation
remains recorded separately from the card acceptance.

The whole application window suite, live Workspace, native operating-system
pickers, Windows/macOS and complete shell pixel equivalence were not run for
this change. The visible picker control uses Qt's existing test fallback dialog.
