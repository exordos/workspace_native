# Composer media, time and saved snippets

The composer emoji button opens a single media popover. Its Emoji tab reuses
the category, search and skin-tone controls from the installed native emoji
catalogue. Stickers has a searchable pack view. The pinned sticker store
starts empty and has no server-side pack loader. A selected entry appends
`[sticker:packId:stickerId](url)` to the draft. The standalone web panels
measured 320 and 340 px wide, respectively, with a 320 px content area. Native uses the requested 360 px popover height. The deployed web popover measured 367 px including its tabs and borders.

Insert date and time opens a local date and time picker with seconds.
Insertion replaces the saved editor selection with the canonical UTC Markdown
time reference and puts the caret after it. Escape restores the editor and
selection. The initial time is one minute after opening, as requested in
issue #128. Insert compares the selection with the opening instant. A valid selection can still be inserted after the clock passes it. During a repeated daylight-saving hour, the default preserves the selected UTC offset. The observed deployed web control starts at the current second.

Saved snippets exposes the generic composer's filter, empty list and create
action. Creation reports that snippets are unavailable. This is a local
placeholder without a legacy request or persistent list. The pinned Workspace
chat disables the generic saved-snippets capability and the deployed toolbar
hides its button. The native toolbar exposes the requested placeholder.

The reference is `workspace_ui` at
`61dacef78dab2f5d0be76f283bebf49fe29b84af`, including the composer media
popover, time picker and saved-snippets dialog and model.
