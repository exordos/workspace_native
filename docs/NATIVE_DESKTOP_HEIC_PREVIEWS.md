# HEIC/HEIF previews in the native client

Status: draft (design first commit for issue #638).
Reference oracle: `workspace_ui` at `e0194ffb667986b58a12256ce41ca5dc99b56b83`,
feature commit `1b9f323b9113c3c1a6c30dcc08c11490777796ea` ("Show HEIC previews in
chat (#433)"). Parity-preserving clone; no restyle of existing surfaces.
Figma token unavailable on this host - mockups not consulted; the reference code is
the design oracle (surfaces are pre-existing chat bubbles, media viewer, composer).

## Scope (reference contract summary)

HEIC/HEIF image attachments get an inline decoded preview in three surfaces -
chat bubble thumbnail, media viewer, composer attachment preview - while the
authorized ORIGINAL bytes stay the download/saved artifact. Decode failure,
unsupported encoding, or an oversize file (> 25 MiB) hides only the preview and
falls back to the file card / `display-error` viewer slot with download enabled.

## 1. Detection (pure, no Qt)

Port of `packages/web/src/shared/lib/heic-preview-format.lib.ts`:

* MIME fast path (`isHeicAttachment`): MIME set `image/heic`, `image/heif`,
  `image/heic-sequence`, `image/heif-sequence` (strip `;` parameters, lowercase).
  A present MIME outside the set that is not `application/octet-stream` or
  `binary/octet-stream` means NOT HEIC. Only a missing MIME or those two binary
  values fall back to the filename regex `\.hei[cf]$` (case-insensitive, trimmed).
* Content sniff (`hasHeicFileType`): read the first `FTYP_HEADER_LIMIT = 4096`
  bytes; require >= 16 bytes and `ftyp` at offset 4; handle the 64-bit `size == 1`
  header (box size at 8, brands at 16, otherwise brands at 8); reject
  `boxSize < brandOffset + 8`, `boxSize > headerLength`, or `boxSize % 4 != 0`.
  (`brandOffset + 8` is room for the major brand PLUS the minor-version field:
  an ftyp box without them is rejected even when the brand itself matches.)
  Major brand in `avif|avis|jpeg` -> not HEIC. Any brand in
  `heic heix heim heis hevc hevx hevm hevs` -> HEIC. Else major brand in
  `mif1 msf1` and no `avif|avis|jpeg` compatible brand -> HEIC. Otherwise not.
* Order (reference `buildWorkspaceMediaDisplayBlob`): metadata predicate first,
  then the cheap content sniff once bytes are in memory.

New module `packages/native/src/heif-preview.{h,cpp}` exposing
`heifLooksLikeHeic(fileName, contentType)` and `heif sniffFtyp(const QByteArray&)`
plus the caps constants; no network, no Qt widgets - unit-testable in isolation.

## 2. Decode backend

The reference bundles libheif 1.23.7 + libde265 1.1.3 as WASM
(`packages/web/vendor/heic/`, LGPL 3.0). The native client links the SYSTEM
libraries instead (same precedent as `libmd4c`):

* build dep: `libheif-dev` (Ubuntu 26.04 provides 1.21.2; detection is library-independent),
* runtime decode of HEVC-encoded HEIC via the libde265 plugin
  (`libheif1` + `libheif-plugin-libde265` + `libde265-0`),
* CMake `pkg_check_modules(LIBHEIF libheif)`; the feature is NOT optional - a
  missing libheif fails configure exactly like missing md4c, so release binaries
  never silently lose previews.
* Add both packages to the apt sets in `.github/workflows/linux-release.yml` and
  to the packaged runtime (patchelf-bundled libs plus the libheif plugin
  directory; verify plugin dlopen from the installed runtime in
  `verify-installed-runtime.py`).
* Licensing: LGPL shared linking; record libheif/libde265 in the about/license
  dataset (the `native-about-policy` / `licenses-dataset-test` catalog must name
  both, mirroring the reference `LICENSES.md` + bundled `heic-license.txt`).

Decode call shape: `heif_read_from_memory` -> `heif_context_get_primary_image_handle`
-> `heif_decode_image` (RGBA) -> `QImage` (copy, `Format_RGBA8888` then
`convertToFormat_ARGB32` semantics as today). Rotation follows the EXIF orientation
already applied by the image pipeline; natural size reported post-decode.

## 3. Caps, queue, cancellation (reference `heic-preview.lib.ts`)

* `MAX_FILE_BYTES = 25 MiB`: files above it never start a decode (file card /
  `display-error`, download unaffected).
* 15 000 ms decode timeout. The reference kills the worker; natively the decode
  runs on the existing `MessageImages` decode thread (already a dedicated
  `QThread` with a serial pipeline): a `QTimer` watchdog at 15 s marks the job
  Failed if the decode has not reported (parity of user-visible behaviour; the
  worker thread is not killed - see Deviations).
* HEIC decodes share the single serial decode lane (no parallel HEIC storms);
  an in-flight preview is cancelled by scope invalidation (sign-out / project
  switch / viewer close) exactly like the current image jobs' scope leases.

## 4. Surfaces

1. **Chat bubble thumbnail** (`message-images.cpp` pipeline + provider): when the
   attachment is HEIC, `Decode` phase routes bytes through the libheif path
   instead of `QImageReader`; `decoded()` carries the converted RGBA image; the
   per-message display image is cached once (reference WeakMap keyed by original
   blob -> native cache keyed by file uuid + revision inside `MessageImages`
   caches). Failure keeps the existing `Failed` -> file-card card state.
2. **Media viewer** (`media-viewer.{cpp,h}` + `qml/NativeMediaViewer.qml`):
   open shows the decoded display image; decode failure marks the slot via the
   existing `markSlotDisplayError` (reference `kind "display-error"`), the
   download action stays enabled and saves ORIGINAL bytes through `FileClient`
   (never the decoded image). Toolbar/i18n are pre-existing (`mediaViewer.download`).
3. **Composer attachments** (`native-image-input.h`, composer attachment preview):
   `.heic`/`.heif` accepted as `urnType image` without local dimension probing
   (reference forces server metadata and skips probing for HEIC); the composer
   thumbnail decodes through the same path with per-attachment cancellation.

## 5. States / i18n

Loading: existing thumbnail skeleton while decoding. Error: file card in bubble,
`display-error` in viewer, download still enabled. Oversize (> 25 MiB): same as
error path but never decodes. No new user-visible strings are expected (all
reference strings pre-exist in both locales); if implementation needs one, add it
to `packages/native/translations/workspace_ru.ts` in the same PR.

## 6. Test plan (TDD)

* New unit group `native-native-heif` (detector + caps): every row of the brand
  table - `heix` yes; `mif1` alone yes; `mif1` + `jpeg` no; `avif` no; `jpeg`
  ftyp no; `heic-sequence` MIME yes; `.heic` name with octet-stream yes; `.heif`
  name with `image/png` MIME no; oversized ftyp header cases per the reference
  boundary checks.
* `native-native-media`: a committed minimal `.heic` fixture (libheif+libde265
  encoded, < 50 KiB) decodes to its known dimensions through the MessageImages
  pipeline; oversize and corrupt-byte cases fail to `Failed` without refetch
  storms.
* `native-native-window`: bubble thumbnail renders for the HEIC fixture; viewer
  opens it, download saves ORIGINAL bytes (sha256 of fixture match); corrupt
  fixture shows the file card and the viewer `display-error` with download enabled.
* CI note: the new CTest group raises the local gate group count by one
  (the full-gate shards pick it up automatically via labels).

## Deviations from the reference (recorded)

* System libheif/libde265 instead of a vendored WASM build - the native process
  links native libraries; detection values (caps, timeouts, brand table) are
  cloned verbatim from the reference source above.
* 15 s timeout marks the job failed instead of terminating a worker: the shared
  decode thread must survive one stuck codec call; if libheif ever blocks longer
  than the timeout the user sees the file-card fallback while the thread finishes
  (documented risk, libheif decode is pure computation with no network wait).

## Open items for the implementation PR

* Verify installed runtime resolves the libde265 plugin from the bundled tree.
* Confirm viewer download path is untouched by display-blob substitution.
* Grep for tests asserting image MIME whitelist before enabling composer HEIC input.
