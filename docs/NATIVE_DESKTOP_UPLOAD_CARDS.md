# Composer attachment upload cards

## Reference and current native boundary

The current comparison target is `workspace_ui` `origin/master` at
`75bd21f37de94728e2ebe100acfea24ac9c1a4e1`. The issue's older reference is
`61dacef78dab2f5d0be76f283bebf49fe29b84af`. The comparison uses the clean Git
tree, not the separately modified UI working directory.

- `workspace-composer-attachments.model.ts` validates each file before queuing,
  runs at most five uploads, and blocks sending for every state other than ready,
  including error. Validation errors cannot be retried; upload errors can.
  Removing a ready file or discarding its scope performs best-effort deletion.
- `message-composer-controlled-attachments.ui.tsx` and
  `shared/ui/attachment-card/attachment-card.ui.tsx` define the 240×58 card,
  validating, queued, progress, ready and error states, individual cancel,
  remove and retry controls, and the accessible labels. The source localizes
  these through `attachmentCard` in `i18n/locales/{en,ru}.json`.
- `message-composer-upload.hook.ts` only accepts file drops and highlights the
  composer. `message-composer.ui.tsx` accepts clipboard files and prevents the
  normal text paste only when files are handled. `message-composer-body.lib.ts`
  preserves real filenames and assigns `pasted-image-YYYYMMDD-HHMMSS.png`
  names, with `-2`, `-3`, etc. for collisions, to anonymous clipboard images.
- The original keeps uploaded attachments separate from visible composer text,
  assembles canonical Markdown on send, supports inserting ready images into
  text by dragging a thumbnail, and transfers card ownership on send. Its tests
  in `chat-page-workspace.test.tsx` check those contracts.

Native keeps verified uploads in separate cards and transfers their serialized
links to the existing durable outgoing queue on send. Missing attachment links
precede the text. When the first paragraph is a standalone quote, the quote
stays first, followed by the missing links and the answer. A link already in
the body is kept at its original position. A ready image can also be
dragged into the composer as a readable alias; the alias is kept in memory,
omitted from the saved draft, and replaced with the canonical image reference
in the submitted body. The card indicates an image in text and can remove it
from text without removing the attachment. Aliases stay stable when another
same-named image is removed. Switching conversation or composer session discards
unsubmitted cards and cancels pending transfers. Local tests use synthetic files
and HTTP responses. They do not establish live-service behavior, exact
cross-renderer pixels, native OS picker behavior or Windows/macOS acceptance.

For clipboard file URLs, native deliberately adds a confirmation before reading
or uploading file contents. A clipboard owner can replace expected text with a
file URL, so an ordinary paste otherwise sends a local file without an explicit
file choice. On Linux, a worker inspects metadata and shows the selected path and
resolved symbolic-link target. After confirmation it opens the verified target,
checks its path and identity, then copies it to a private temporary file; a file
that changed since the path was shown is rejected. One paste is limited to 100 files
and 100 MiB total, in addition to the existing 25 MiB per-file limit, to bound
temporary storage. Cancellation or a change of account, conversation or edit
session discards the selection and removes temporary files once copying stops.
Temporary files are removed on normal cleanup, but a process or machine crash can
leave a private file in the system temporary directory after confirmation.

This verified clipboard-file path is Linux-only. On Windows and macOS, pasting
file URLs is unavailable and users must choose files with the picker. This is a
functional difference from the web composer; those platforms have not been
validated. Clipboard image data continues to paste directly, and the file picker
and external file drop remain explicit file selections.

Like the reference composer, visible image aliases are matched lexically: typed
text matching an attachment alias is serialized as that image, including a
matching token in code or reference-style Markdown. This preserves the source
behavior but means users should avoid an ambiguous matching token while that
attachment is present; a future change needs to address the same behavior in
both clients.

## Cleanup identity boundary

At `workspace_backend` `origin/master` `8259c22a87018dc41064343e86c8d756edb30400`,
`WorkspaceFileController._create_from_multipart` creates a fresh UUID and binds
the file to the requesting project, user and stream. `delete` uses the fixed
`/files/{uuid}` route and `get_workspace_owned_file` filters UUID, project and
user. Successful deletion removes the owned metadata and access; object bytes
are removed only when the server finds no remaining storage reference.

The client may request cleanup only for its own accepted upload result, after
matching stream, name, MIME, size and checksum, on the same owner, API generation and file-service epoch. Production wiring
constructs the `FileClient` and controller with the same `ApiClient` in
`packages/native/src/main.cpp`. Arbitrary references in draft or
message Markdown are never deletion authority. A file named by an admitted or
uncertain send, or by an already submitted edit, must not be deleted based on
a failed or missing acknowledgement.

When reopening an edited reply, native extracts standalone attachments before
restoring quote tabs, matching the source composer. It preserves the original
attachment prefix when the user saves the restored reply again. A new upload
in a reply starting with a standalone quote without a snapshot follows the
quote, with a blank line before the attachment and before the answer. On
reopening, that attachment belongs to the quoted tab. The editor presents the
answer without the separator and restores the separator when saving.

A link shown as a Markdown code example or escaped text keeps its literal form. The ready upload still inserts an actionable link.
