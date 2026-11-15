# Image attachment validation

Issue #191 follows the pinned desktop attachment validation contract
(`workspace_ui` 61dacef78dab2f5d0be76f283bebf49fe29b84af,
`shared/lib/validation.ts` and the composer attachment model).

The native file picker supplies a path rather than a browser File MIME type.
For PNG, JPEG, GIF and WebP filenames, infer the declared type from both the
selected filename and the sanitized attachment name sent to the server. A caller
must not bypass validation by uploading a non-image local filename as an image.
Reject a mismatch before making a network request. This checks image signatures,
not full image decoding.

For image filenames, capture the bounded file in one in-memory buffer, then
validate its signature. Derive the checksum and content type from those same
bytes, and retain the read-only buffer as the multipart body. Editing or replacing
the local file after capture cannot change the uploaded bytes. The existing
25 MiB per-file limit bounds each retained snapshot. The controller admits at
most five uploads, so active image data uses at most 125 MiB, in addition to
transport buffers and replies awaiting deferred deletion. The multipart reply
owns each buffer and releases it through the existing completion, cancellation
and session invalidation paths. No temporary disk copy or new cleanup mechanism is needed.
Other file types retain the existing bounded file streaming behavior. Keep the
current empty/size bounds, owner fencing and authenticated upload.

The existing localized attachment error surface reports the validation failure;
no retry operation or new upload queue is introduced. Regression checks must
prove that a mislabeled image produces no POST, that a matching image is still
uploaded, and that the English/Russian visible error survives both palettes.
