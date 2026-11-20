# Composer preview

The expanded formatting toolbar has one eye button. Its accessible name is
Preview while writing and Write while previewing. Preview replaces the editor
with a scrollable region and keeps the current draft and toolbar expansion.
The collapse button is disabled until the user returns to Write. An empty
draft displays “Nothing to preview yet”. Sending returns to the editor.

The viewport is 40 px high for an empty draft and grows to 128 px for longer
content. Beyond that height the body scrolls inside the composer. A rendering
error uses the renderer’s localized error text. Rendering runs synchronously.

The native preview renders the body through the same Markdown renderer used
by a message. The controller supplies the body it would send or save, including
mention serialization, reply blocks and ready attachments. An existing inline
attachment keeps its place. A missing attachment link appears once.

The pinned web composer constructs two different strings when a ready file's
marker is missing. The preview in `message-composer.ui.tsx` appends the file to
the text. Submission goes through `useWorkspaceComposerAttachments.handleSend`
and `prependWorkspaceComposerAttachmentMarkdown`: it puts the file before
ordinary text, or between a standalone quote and its answer. Native preview
follows the submitted order in both cases so the user sees what will be sent.
The reference source is `workspace_ui` at
`61dacef78dab2f5d0be76f283bebf49fe29b84af`.

With an empty draft in the deployed web composer, the 32×32 toggle opened a
40 px scrollable region with 14 px text. Write returned to the original empty
editor and restored the collapse control.
