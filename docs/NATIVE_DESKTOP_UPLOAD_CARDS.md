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
links to the existing durable outgoing queue on send. A ready image can also be
dragged into the composer as a readable alias; the alias is kept in memory,
omitted from the saved draft, and replaced with the canonical image reference
in the submitted body. The card indicates an image in text and can remove it
from text without removing the attachment. Aliases stay stable when another
same-named image is removed. Switching conversation or composer session discards
unsubmitted cards and cancels pending transfers. Local tests use synthetic files
and HTTP responses. They do not establish live-service behavior, exact
cross-renderer pixels, native OS picker behavior or Windows/macOS acceptance.

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
