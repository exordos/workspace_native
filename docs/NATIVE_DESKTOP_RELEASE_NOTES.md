# Workspace Native 0.1.4 — Linux development preview

A picked image now displays after Send when the draft already contains a caption.
Previously, upload and message confirmation could succeed while its preview
remained missing. The fix keeps the image reference in its own Markdown
paragraph. Ordinary file/video attachments retain their serialized behavior.

The actual file-dialog/send/readback/pixel regression, complete Controller830
and Window283 suites, eight Source encrypted workflow cases, installer/version
checks and local packaged image/crypto/TLS probes passed. The public archive and
actual installed 0.1.3 → 0.1.4 update are verified separately after publication.

This remains a Linux development preview. Full feature/pixel parity, Source
image-first composer ordering, live-account acceptance, the complete media
viewer, Windows and macOS remain unfinished.

---

# Workspace Native 0.1.3 — Linux development preview

Standalone image paragraphs now show static previews between surrounding text
and captions. PNG, JPEG, GIF and WebP use a bounded shared preview cache; returning
to a warm chat avoids another file transfer. A failed preview retains the existing
Save action for the original attachment. Encrypted bodies load their images only
while readable; locking hides them and signing out clears the preview scope.

The complete current window suite passed 282 cases. Focused checks cover actual
image pixels, repeated navigation, session replacement, a failed preview's Save
dialog and the existing independent Source encrypted-message workflow. These
use synthetic local services; full live-account and desktop parity remain open.
Animation, mixed/multiple-image composition and the complete media viewer are
unfinished. Preview pixels are currently cached in memory; cold offline image
restoration after restart is not yet accepted.

Existing 0.1.1 and newer installations automatically download a found update and
offer **Install and restart**. The actual public 0.1.2 → 0.1.3 update passed on the
current host: automatic download, one install action, restart into the new binary,
normal quit and preservation of one synthetic cache row. The new version's update
check succeeded without a manual retry. This anonymous check does not establish
authenticated history or credential preservation. Earlier intermittent
metadata-check errors remain unexplained.

Download and extract the archive, then run `bash install.sh`. The per-user
installation includes Qt and needs no administrator privileges. This preview
targets Ubuntu 26.04 x86_64 / glibc 2.43 or newer. Other distributions and operating
systems remain unaccepted. The typed disk cache defaults to a configurable
decimal 10 GB; the shared image preview cache has a separate 32 MiB memory bound.
Keep an existing client available for unfinished workflows.
