# Workspace Native 0.3.5 - Linux development preview

Session credential storage is now proven against a regular desktop profile, not
only a disposable test keyring. The opt-in secure-store acceptance group
(`native-secure-store-platform`, enabled with
`-DWORKSPACE_NATIVE_PLATFORM_TESTS=ON`) gained
`persistentProfileSurvivesKeyringDaemonRestart`: a real GNOME Keyring daemon
serving a durable on-disk profile must keep a saved sign-in token available
across its own restart, both before and after rotation, and a falsification
control proves reads come from the on-disk keyring rather than daemon memory.
Both platform groups also passed against the host's real persistent login
keyring. No user-visible behavior changed; this release is verification and
regression coverage (acceptance record in `docs/NATIVE_DESKTOP_CRYPTO.md`,
locked-keyring sessions tracked in issue #28).

Existing installations download available updates automatically and offer
**Install and restart**. For a new per-user installation, extract the archive
and run `bash install.sh`.

[Full changelog: 0.3.4...0.3.5](https://github.com/exordos/workspace_native/compare/0.3.4...0.3.5)

---

# Workspace Native 0.3.4 - Linux development preview

Messages are now shareable. The message context menu gained **Copy link**,
which puts the message's canonical web URL
(`<server>/org/<organization>/project/<project>/message/<message>`) on the
clipboard so a colleague can open that exact message. Opening an `ew://`
deep link activates the running window (a second launch forwards the link
to the primary instance) and navigates to the target: the topic opens, the
message scrolls into view and its row is briefly highlighted; a message
outside the loaded window is reached through bounded backward pagination.
Every incoming link is validated strictly - scheme, route shape, unsafe
characters and the message UUID - and a link for a foreign organization or
project, or for an unavailable message, lands safely with a notice instead
of navigating. The contract and its validation rules are documented in
`docs/NATIVE_DESKTOP_MESSAGE_LINKS.md`.

Existing installations download available updates automatically and offer
**Install and restart**. For a new per-user installation, extract the archive
and run `bash install.sh`.

[Full changelog: 0.3.3...0.3.4](https://github.com/exordos/workspace_native/compare/0.3.3...0.3.4)

---

# Workspace Native 0.3.3 - Linux development preview

An open conversation now stays coherent when a whole topic is renamed or moved to
another channel. The follow previously kept the destination usable but left the
vacated route's cached history readable offline, so revisiting the old channel
could show messages that no longer belong there. The vacated topic bucket and
the former channel's aggregate bucket are now dropped as an authoritative
membership change, mirroring the deleted-conversation rule; unrelated cached
windows are untouched, the followed window keeps its loaded messages and
pagination boundaries, and a rename storm over the receiving channel neither
duplicates nor drops messages or topic rows. The follow contract and its new
regression coverage are documented in `docs/NATIVE_DESKTOP_TOPIC_NAVIGATION.md`.

Existing installations download available updates automatically and offer
**Install and restart**. For a new per-user installation, extract the archive
and run `bash install.sh`.

[Full changelog: 0.3.2...0.3.3](https://github.com/exordos/workspace_native/compare/0.3.2...0.3.3)

---

# Workspace Native 0.3.2 - Linux development preview

This release changes verification only; product behavior, storage and APIs are
unchanged from 0.3.1. The encrypted UI test group timeout of 360 seconds was
tuned on a GPU-equipped host and timeout-killed healthy runs on GPU-less
hosts, which looked like an order-dependent test stall. Measurements on the
reference stand show the explicit group needs at most 548 seconds (raised to
720 with headroom) and a direct unfiltered binary run executes both encrypted
suites in one process in 893 seconds; both budgets are now recorded in the
development documentation. The full local suite is green on this Ubuntu 26.04
reference host with the bare Xvnc plus openbox stand.

Existing installations download available updates automatically and offer
**Install and restart**. For a new per-user installation, extract the archive
and run `bash install.sh`.

[Full changelog: 0.3.1...0.3.2](https://github.com/exordos/workspace_native/compare/0.3.1...0.3.2)

---

# Workspace Native 0.3.1 - Linux development preview

This release changes verification only; product behavior, storage and APIs are
unchanged from 0.3.0. The window geometry checks previously pinned one
maintainer machine's font metrics and failed on other font stacks. The status
dialog oracles now derive the wrapped description line count at runtime, and
font-derived heights and text-sized widths use documented small tolerances.
The full window suite is green on this Ubuntu 26.04 reference host with the
bare Xvnc plus openbox stand.

Existing installations download available updates automatically and offer
**Install and restart**. For a new per-user installation, extract the archive
and run `bash install.sh`.

[Full changelog: 0.3.0...0.3.1](https://github.com/exordos/workspace_native/compare/0.3.0...0.3.1)

---

# Workspace Native 0.3.0 — Linux development preview

Ordinary conversation drafts now survive restart and synchronize with Workspace.
The Drafts activity opens the exact saved draft and offers server/local/delete
choices when another client changes it. Failed deletions remain available for
retry. Sending consumes only the confirmed snapshot; newer input stays editable.

Local input coalesces over 400 ms and remote saves over one second. Navigation
uses local storage, with bounded clean-memory residency and 100-card preview
pages. Refresh waits for an active save. Unsent work is kept separately from the
configurable decimal 10 GB history cache and is not evicted by its LRU.

Native quotes remain part of saved Markdown. Source reply tabs and inline-edit
sessions remain unfinished. Encrypted draft text keeps its existing memory-only
behavior. Complete feature/pixel parity, other Linux distributions, Windows and
macOS remain unfinished. This preview targets Ubuntu 26.04 x86_64 / glibc 2.43 or
newer. Remembered sign-in requires a working operating-system credential store.

Existing installations download available updates automatically and offer
**Install and restart**. For a new per-user installation, extract the archive
and run `bash install.sh`.

[Full changelog: 0.2.0…0.3.0](https://github.com/exordos/workspace_native/compare/0.2.0...0.3.0)

---

# Workspace Native 0.2.0 — Linux development preview

Reopening the application now restores the last verified saved account, project
and conversation. A moved topic opens at its current server location. Explicit
navigation takes priority over pending restoration; an unavailable saved chat
does not silently select another conversation.

Remembered sign-in requires a working operating-system credential store. Accounts
saved by earlier previews retain **Continue**; using it successfully once selects
that account for subsequent automatic startup. Existing credential storage and
the cache schema remain compatible. Published versions retain their original tags.

Complete local session, vault, controller, preferences, window and encrypted UI
checks passed. Packaged Linux checks exercised actual OTP sign-in, automatic
account/project/conversation restoration in new processes with a real OS store,
ordinary message send/read-back and automatic incoming-message display. Matched
unedited source/native frames retain the remaining visual differences. These
checks do not establish permanent credential-provider setup on other desktops,
authenticated upgrade preservation or complete application parity.

The downloaded public 0.2.0 package also passed primary OTP sign-in and a fresh
process restored the account, project and conversation without manual input.
An actual installed 0.1.6 to 0.2.0 update passed automatic download, one
**Install and restart** click, normal process exits, same-primary activation
and preservation/reopen of one synthetic history-cache row. Both checks used
the exact published binary; authenticated upgrade preservation remains open.

Existing installations download available updates automatically and offer
**Install and restart**. Download and extract the archive, then run `bash install.sh`
for a new per-user installation. This preview targets Ubuntu 26.04 x86_64 /
glibc 2.43 or newer. The configurable disk cache defaults to 10 GB. Other
distributions, Windows, macOS and full feature/pixel parity remain open.

[Full changelog: 0.1.6…0.2.0](https://github.com/exordos/workspace_native/compare/0.1.6...0.2.0)

---

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
