## What

Test-only regression pins for the three secondary-page hover sites that issue #945 (audit #942 follow-up) flagged as dead sibling-MouseArea hovers:

- `inboxRowFillsOnLabelCenterHover` - inbox row fill/border at the row title center (`NativeInboxPage.qml` `rowArea`, :219),
- `starredCardFillsAndRevealsActionsOnLabelCenterHover` - starred card fill and the hover-revealed action cluster at the author line center (`NativeStarredPage.qml` `cardHover`/`actionsHover`, :239/:325),
- `sessionSwitcherRowFillsOnLabelCenterHover` - remembered-session row fill at the primary-label center (`SessionSwitcher.qml` `rowMouse`, :207).

Each pin moves the pointer to the row label center (the claimant point that killed the #944 chip hover) and asserts the hover-driven fill/border/reveal lights and reverts from a neutral corner. One test-only seam: `objectName: "starredActions-" + id` on the starred actions zone so the reveal has a stable handle.

## Why there is no migration (the claim did not reproduce)

The acceptance contract demanded RED-before-migration witnesses. All three witnesses measure GREEN at tip `c01f130b` with the existing MouseArea wiring untouched (solo runs, WM'd display, portal pair, Qt 6.10.2):

| case | measured |
|---|---|
| `inboxRowFillsOnLabelCenterHover` | 1470.5 ms PASS |
| `starredCardFillsAndRevealsActionsOnLabelCenterHover` | 1581.0 ms PASS |
| `sessionSwitcherRowFillsOnLabelCenterHover` | 812.0 ms PASS |

Root-cause note: #944 was dead because the copy chip was mounted outside its ancestor bounds by a stale `positionToRectangle` offset, which excluded it from Qt's pointer-delivery walk, and its label layer claimed the pick. On these three pages the fill MouseArea is properly anchored inside the row and is itself the top claimant under the label, so `containsMouse` fires from the label point and hover is alive. A hover migration was authored but reverted per the reproduce-first rule - there is nothing to fix at these sites today.

The pins keep that verdict enforced: if a future relayout or a label-layer claimant kills any of these hovers the way #944's did, the owning case goes RED at the window layer.

## Buckets

`gen-test-buckets.py` regenerated with the three measured QtTest xmls: 608 functions, 27 buckets kept (max bucket 109.5 s, ceiling 120 s), fragments updated for part11/part14/part18.

## Scope statement

No C++/QML product behaviour changed (single added `objectName` seam only); three new window cases are the deliverable. The full-gate CI runs the complete suite including these cases.
