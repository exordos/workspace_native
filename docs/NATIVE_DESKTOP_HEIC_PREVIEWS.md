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
  to the packaged runtime: CMake installs the `libheif-libde265.so` plugin into
  `${CMAKE_INSTALL_LIBDIR}/heif-plugins` (i.e. `lib/heif-plugins` on the release
  distro) and the `libde265.so.0*` runtime libraries into `${CMAKE_INSTALL_LIBDIR}`
  (they are dlopen'd, so Qt's ELF deployment never sees them);
  `FinalizeLinuxRuntime` gives `libheif`/`libde265` the package `$ORIGIN` and
  the plugin tree `$ORIGIN:$ORIGIN/..` so their transitive dependencies resolve
  inside the package; the installed runtime probe (`native-runtime-probe`, run
  by `verify-installed-runtime.py`) decodes the HEVC fixture and checks the
  mapped plugin paths, so a package that cannot dlopen its bundled libde265
  fails the release gate.
* Licensing: LGPL shared linking; record libheif/libde265 in the about/license
  dataset (the `native-about-policy` / `licenses-dataset-test` catalog must name
  both, mirroring the reference `LICENSES.md` + bundled `heic-license.txt`).

* Decode call shape: `heif_read_from_memory` -> `heif_context_get_primary_image_handle`
  -> `heif_decode_image` (RGBA) -> `QImage` (copy, `Format_RGBA8888` then
  `convertToFormat_ARGB32` semantics as today). Rotation follows the EXIF orientation
  already applied by the image pipeline; natural size reported post-decode.
* Plugin registration (implementation finding, libheif 1.21.2 Ubuntu): the
  library does NOT auto-register its codec plugins from
  `LIBHEIF_PLUGIN_DIRECTORY`; without an explicit `heif_load_plugins` pass the
  HEVC decoder is invisible and every decode fails. `heifDecodeRgba` runs it
  once per process: the deploy-bundled tree next to the executable is tried
  first and is authoritative when it registers a plugin; its bin-relative path
  is compiled from the configured install layout
  (`WORKSPACE_NATIVE_HEIF_PLUGINS_SUBDIR`, derived from
  `${CMAKE_INSTALL_LIBDIR}/heif-plugins`, #670 finding 2) so a `lib64` package
  searches the directory the packaging actually writes; otherwise every
  directory `heif_get_plugin_directories()` reports
  (built-in path + `LIBHEIF_PLUGIN_PATHS`) is scanned, which is what an
  unpackaged development build uses. The installed-runtime check in the probe
  (see decode backend above) covers the bundled-tree path.
* Scope refinement: `REQUIRED` at configure time applies to Linux (the shipping
  release platform). The macOS/Windows `desktop-platform-packages` jobs do not
  provision libheif yet, so non-Linux builds compile a decode fallback that
  reports "libheif not available" (file-card behavior); provisioning libheif
  there is tracked with the packaging items below.

## 3. Caps, queue, cancellation (reference `heic-preview.lib.ts`)

* `MAX_FILE_BYTES = 25 MiB`: files above it never start a decode (file card /
  `display-error`, download unaffected).
* Pixel budget (`kHeifMaxDecodePixels = 50 MP`): checked on the handle BEFORE
  decoding (#670 finding 3). Sized to admit ordinary high-resolution photos -
  a 48 MP iPhone Pro frame (8064x6048 = 48.8 MP) decodes; the worst-case
  RGBA8888 canvas stays bounded at 200 MB. libheif 1.21 exposes no scaled-decode
  option for libde265, so the budget bounds memory rather than the decode.
* 15 000 ms decode timeout. The reference kills the worker; natively the decode
  runs on the existing `MessageImages` decode thread (already a dedicated
  `QThread` with a serial pipeline): a `QTimer` watchdog at 15 s marks the job
  Failed if the decode has not reported (parity of user-visible behaviour; the
  worker thread is not killed - see Deviations). The watchdog does not free the
  job's pipeline accounting: the ticket stays counted until the serial worker
  actually retires it, so a stalled decode cannot make `drain()` pile further
  encoded payloads onto a busy worker (#670 findings 1/4).
* The media viewer HEIC render is bounded by the same 15 s watchdog (#670
  finding 2), armed when the serial worker STARTS the render (#670 re-review):
  queue time behind an earlier render never consumes a slot's timeout. A
  stalled render settles the slot as `display-error` (uuid stays downloadable,
  reference `replaceItem`), and the late render result is discarded by the
  existing generation/state fences.
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

## Composer urn semantics (this round)

Reference `buildWorkspaceComposerAttachmentMetadata`
(`workspace-composer-attachments.lib.ts:136-156` at e0194ff): an
`isHeicAttachment(name, content_type)` hit forces the urnType `image`
regardless of the content type family (the server may echo
`application/octet-stream` when the browser/host MIME database does not know
the container) and skips the local dimension probe, so a HEIC urn never
carries `w`/`h`.

Native transplant (`FileMetadata::heicAttachment()` / `urnKind()` in
`file-client.cpp`): `FileMetadata::urn()` names the type through `urnKind()`
(HEIC forces `image`) and omits `w`/`h` for HEIC even if a probe result is
present; `FileClient::upload` starts the upload directly for HEIC instead of
running the bounded `QImageReader` probe (the probe can never succeed for
HEIC on Qt without a heif plugin, and must not leak dimensions if one ever
ships). The composer Markdown `!` prefix follows `urnKind()` too, mirroring
the reference `urnType === "image" ? "![...]" : "[...]"` rule. Pinned by
`file-client-test`: `heicAttachmentUrnSemantics` (urn table incl. octet-stream
and empty MIME, `.heif` extension, `image/heic-sequence`, non-HEIC rows
unchanged) and `heicUploadForcesImageUrnWithoutProbing` (fixture bytes go up
unmodified, echo guard passes, finished urn is `image` without dimensions).

## Deviations from the reference (recorded)

* System libheif/libde265 instead of a vendored WASM build - the native process
  links native libraries; detection values (caps, timeouts, brand table) are
  cloned verbatim from the reference source above.
* 15 s timeout marks the job failed instead of terminating a worker: the shared
  decode thread must survive one stuck codec call; if libheif ever blocks longer
  than the timeout the user sees the file-card fallback while the thread finishes
  (documented risk, libheif decode is pure computation with no network wait).
* `NativeMessageImage` re-enters its lease `refresh()` via `Qt.callLater` when
  `loadActive` changes. HEIC urns carry no `w`/`h` hints (composer urn
  semantics), so the decoded size publishes through the unknown-size fallback
  (240x180 -> 48x32); a synchronous refresh during the `loadActive` notify moves
  `height`/`viewportY` - `loadActive`'s own dependencies - and Qt flags a binding
  loop, freezing the projection at a stale value. The next-event-loop re-entry
  keeps the lease contract identical (acquire is already asynchronous).
  Window case `heicImagePaintsBubbleViewerRenderAndDownloadSavesOriginal`
  reproduces the loop as a QtTest warning if the handler is made synchronous.

## Window-layer cases (shipped with this issue)

* `heicImagePaintsBubbleViewerRenderAndDownloadSavesOriginal`: bubble paints the
  fixture's solid #4682b4 inside the inline-image bounds (grabWindow pixel
  probes); the viewer slot lands ready on `slot-0-heif.png` (48x32, decoded
  colour checked from the render file on disk); the download action saves the
  byte-identical ORIGINAL container (urn WITHOUT w/h is the input contract).
* `corruptHeicFallsBackToFileCardAndDisplayErrorSlotWithDownload`: a container
  that sniffs HEIC but fails the decode (`heicTruncatedFixture` - the
  `heicCorruptFixture` ancillary-box witness DECODES and exists to pin the
  sniff-vs-decode split) settles the bubble Failed (file-card surface), marks
  the viewer slot `display-error` with `kind == image`, keeps the download
  button enabled (reference downloadEnabled), and saves the original corrupt
  bytes verbatim.

## Open items for the implementation PR

* Verify installed runtime resolves the libde265 plugin from the bundled tree.
  Resolved: `verify-installed-runtime.py` asserts `libheif`, `libde265` and the
  `lib/heif-plugins` plugin tree ship inside the package, and
  `native-runtime-probe` decodes the HEVC fixture while its mapped-library
  prefix check keeps `libheif.so`/`libde265.so` and their transitive
  dependencies (libsharpyuv) inside the package, so a package that cannot
  dlopen its bundled decoder leaks to system libraries and fails the
  package-linux gate that caught this round.
* Confirm viewer download path is untouched by display-blob substitution.
* ~~Grep for tests asserting image MIME whitelist before enabling composer
  HEIC input.~~ Resolved: the composer attach dialog carries no MIME/name
  whitelist (`nameFilters` exists only on diagnostics JSON dialogs), so
  HEIC selection reaches `FileClient::upload` unfiltered; the urn semantics
  section above governs the outcome.
