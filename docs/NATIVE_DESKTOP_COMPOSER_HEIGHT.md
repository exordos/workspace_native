# Composer height and resizing

The automatic editor has a 40 px field for one line, grows by 24 px per
additional visual line, and stops at 128 px. In the compact composer, two,
three and four lines measured 64, 88 and 112 px in the deployed web client.
The shell adds 17 px for its padding and top rule. Reply, edit, upload and
formatting rows continue to occupy their own space.

At 64 px of content the top strip exposes a resize handle. The interactive
strip is 20 px high and its visible grip is 56 × 4 px. Dragging or pressing
ArrowUp or ArrowDown on the handle changes the height. Arrow keys move 24 px.
A short draft can return to automatic height within 1 px of its content
floor. Long drafts allow a 96 px manual floor. A locked height survives
content shrinking until the user brings the field back to its floor.

The expand control is 28 × 28 px. It appears at 96 px and stays through
the intermediate range while the automatic field stays above 64 px. In this
compact layout the attachment and emoji controls move into a vertical rail
on the left of the field. The editable viewport reserves the top-right button’s width even when editing hides the poll action. The full-height composer leaves 4 px of the
conversation area, tracks window resizing, and stays expanded when text is
removed. Collapse restores automatic sizing. At a 900 px window height the
observed conversation area measured 766 px and the expanded shell 762 px.

The source contract is the pinned Workspace UI composer resize hook, layout
and constants in `packages/web/src/widgets/message-composer/` at
`61dacef78dab2f5d0be76f283bebf49fe29b84af`.
