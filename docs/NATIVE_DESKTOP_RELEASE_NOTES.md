# Workspace Native 0.1.6 — Linux development preview

The message composer adds Ctrl+B for bold, Ctrl+I for italic, Ctrl+E for inline
code and Ctrl+Shift+X for strikethrough. A selection is wrapped in Markdown;
with no selection, the caret stays inside the new marker pair. Existing drafts,
focus and scrolling are preserved. The formatting toolbar and remappable
shortcut settings remain unfinished.

The complete local Window293 and encrypted UI142 suites, locale14, independent
encrypted-message checks and Linux package/runtime checks passed. Representative
physical Ctrl+B passed in English and Russian keyboard layouts. Final master and
tag packaging CI passed. The downloaded public archive passed checksum and
deployed runtime checks. An actual installed 0.1.5 to 0.1.6 update passed automatic
download, one Install and restart click, normal exits into the exact new binary,
same-primary activation and preservation/reopen of one synthetic cache row.
No manual update-check retry was needed. AT-SPI inspection diagnostics remain;
authenticated history and credential preservation are separate acceptance work.

This remains a development preview for Ubuntu 26.04 x86_64 / glibc 2.43 or newer.
The configurable disk cache defaults to 10 GB. Full feature/pixel parity, other
distributions, Windows and macOS remain open.

---

# Workspace Native 0.1.5 — Linux development preview

New chat now opens existing active encrypted Direct conversations, including
those started by the other participant. Requesting a key, reading and sending
use the existing encrypted workflow. Missing keys keep Send and Attach locked;
creating new encrypted pairs and bootstrap remain unfinished.

Update checks recover once from an HTTP/2 connection that fails before any
response status, headers or body arrive. The original total timeout and download
rules remain unchanged.

The complete local window and encrypted workflow suites passed, together with
controller, updater, transport, installer and packaged runtime checks. GitHub
packaging CI passed. The published archive passed its checksum and runtime checks. An actual
installed 0.1.4 to 0.1.5 upgrade passed: automatic download, one Install and
restart click, normal exits, same-primary activation and preservation/reopen
of one synthetic cache row. No manual update-check retry was needed. Desktop
diagnostic warnings remain; authenticated history and credential preservation
are separate unfinished acceptance.

This remains a development preview for Ubuntu 26.04 x86_64 / glibc 2.43 or newer.
The configurable disk cache defaults to 10 GB. Live-account acceptance, full
feature/pixel parity, other distributions, Windows and macOS remain open.

---

# Workspace Native 0.1.4 — Linux development preview

A picked image now displays after Send when the draft already contains a caption.
Previously, upload and message confirmation could succeed while its preview
remained missing. The fix keeps the image reference in its own Markdown
paragraph. Ordinary file/video attachments retain their serialized behavior.

The actual file-dialog/send/readback/pixel regression, complete Controller830
and Window283 suites, eight Source encrypted workflow cases, installer/version
checks and local packaged image/crypto/TLS probes passed. The public archive and
actual installed 0.1.3 → 0.1.4 update passed the separate published checks.

The published 0.1.4 archive passed its checksum and deployed runtime checks.
The actual public 0.1.3 → 0.1.4 update passed: automatic download, one Install
and restart click, normal exits into the exact new binary, same-primary
activation and preservation/reopen of one synthetic cache row. The completed
run required no manual update-check recovery. An earlier interrupted attempt
and its manual metadata recovery remain recorded with unresolved causes.
Portal/AT-SPI diagnostics remain; this does not establish authenticated history
or credential preservation, warning-free desktop operation or a transport fix.

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
