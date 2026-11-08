# Native encrypted-chat provider

The latest fully accepted baseline is the imported V34 snapshot. Current ordinary-channel
response changes are in progress; see [current development](CURRENT_DEVELOPMENT.md)
for the checks and unresolved observations on this branch.

[NativeChatCrypto](../packages/native/src/native-chat-crypto.h) implements the
binary operations from the existing
[real client provider](../packages/web/src/entities/messenger/chat-real-crypto-provider.lib.ts).
It uses OpenSSL's HKDF-SHA256, ChaCha20-Poly1305, X25519 and
[HPKE APIs](https://docs.openssl.org/3.5/man3/OSSL_HPKE_CTX_new/), rather than a
new encryption format. OpenSSL 3.2 or later is required; the checked Linux SDK
uses 3.5.5. The Linux staging rules copy matching Crypto/SSL runtime libraries
and their license, including runtime paths for transitive loading.

The native Linux runtime supports reading encrypted history, requesting an exact
missing generation, accepting a correlated grant or rejection, and sending text
to a selected topic or the verified actual default topic. The existing composer
uses the selected sending generation separately from the current chat key.
Uncertain text sends retain their original identity, target and ciphertext;
Check only reads status, while explicit Retry first checks the same placement.
The local HTTPS/WSS and source-client fixtures use disposable identities;
live-service acceptance remains open. An initial issuer workflow now grants or
rejects an incoming request from the chat. Bootstrap, rotation, durable outgoing
messages and complete key-management screens remain incomplete.

## Wire compatibility

The provider retains the [existing E2EE contract](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/docs/E2EE.md#wire-contract-and-cryptography):

- A message is salt32 followed by ciphertext and tag16. HKDF derives key32 from
  the group key, salt and UTF-8 JSON context. Its nonce is twelve zero bytes;
  each encryption obtains a new salt from OpenSSL's cryptographic RNG. The AAD
  contains the exact context bytes followed by salt. There is no new prefix.
- A grant is `WSCG`, version byte 1, HPKE encapsulation32 and ciphertext/tag48.
  HPKE Base uses X25519/HKDF-SHA256/ChaCha20-Poly1305. The context binds stream,
  key generation, request message, sender device and recipient device; AAD also
  binds the header and recipient public key.
- Request keys use the existing raw 32-byte X25519 encoding. Binary envelopes
  remain separate from the REST adapter's padded Base64 strings.

The native provider limits message input to 16 MiB, matching the current native
HTTP response bound. Display counts the source's 40,000 Unicode code points,
including valid surrogate pairs, and keeps the native plain-text Copy result
intact. The text composer also enforces the existing 213,336-character Base64
wire limit; 39,988 astral characters fit that envelope exactly, while a larger
envelope preserves the draft without posting. Actual UUID/device grant contexts fit OpenSSL's 1,024-byte
HPKE info bound; oversized grant contexts fail closed. This does not claim the
source provider's theoretical multi-gigabyte allocation capability.

The provider performs no HTTP, cache writes, owner selection or authorization.
Derived temporary message keys and failed plaintext/key outputs are explicitly
cleansed. Caller-owned key/plaintext buffers and their copies still require an
appropriate lifecycle; this is not a claim of locked memory or hardware-backed
storage. Successful plaintext must remain in scoped presentation memory, and
private request/group keys must never enter the message cache or ordinary API
payloads.

## Verification

The regular [provider tests](../packages/native/tests/native-chat-crypto-test.cpp)
cover binary/Unicode/empty messages, fresh salts, each authenticated context
field, malformed framing, wrong keys and modified ciphertext/tags. They also
decrypt small checked-in
[synthetic source vectors](../packages/native/tests/workspace-chat-crypto-v1-vectors.json).
All keys in that file are disposable test values, unrelated to account storage.
The initial independent provider run passed 19 cases both normally and under
ASan/UBSan/LSan with leak detection and no suppressions.

```sh
ctest --test-dir packages/native/build -R '^native-native-chat-crypto$' --output-on-failure
```

The [cross-client helper](../packages/native/tests/cross-check-chat-crypto.mjs)
executes the actual source provider, after erasing TypeScript with the source
lockfile's esbuild version. It checks exact installed versions of the five
cryptographic dependencies, uses a temporary source copy and removes it afterward.
It does not reimplement the JavaScript algorithms or access account storage.

After installing the web dependencies, run from the repository root:

```sh
node packages/native/tests/cross-check-chat-crypto.mjs \
  --native packages/native/build/native-chat-crypto-test \
  --source-root "$PWD" --dependency-root "$PWD/node_modules"
```

The initial cross-client run passed 28 direction checks: source-to-native and
native-to-source messages, normal/escaped Unicode contexts, and grants using both
source- and native-generated request pairs. It includes 160 kB plaintext messages;
the larger values are exercised at runtime rather than stored in the fixture.
The helper prints only result/provenance metadata. Its optional `--vectors` writes
fresh synthetic fixtures and should be used deliberately, rather than during an
ordinary check.

Live key exchange with authorized request completion, permission changes,
historical generations, key deletion, reconnect and cross-device UI acceptance
remain open. A successful provider exchange does not establish those workflows.

## Initial durable key vault

[NativeChatKeyVault](../packages/native/src/native-chat-key-vault.h) stores a
durable device identity and generation keys in a private, owner-partitioned SQLite
database under application data, separate from the evictable message cache.
Its random 32-byte master is held by the existing OS credential-store adapter,
with plaintext fallback disabled. The SQLite records use the tested binary
provider with a separate `workspace-native-key-vault-v1` owner context and a
`WSKV`/version-1 local prefix; group record addresses also bind device, stream and
generation. This internal format is not a new chat wire format or key export.

Keys are not evicted by the messenger's 10 GB cache budget. This foundation
has a 64 MiB SQLite database page cap and a 2,048-byte plaintext bound for device,
group-key and completion records; pending requests have the separate bounds below.
Growth failures surface instead of silently deleting secrets. Those bounds do
not describe total temporary filesystem use. Key reads/writes are small,
synchronous operations after asynchronous credential initialization; large or
complete encrypted-chat workload performance is not accepted.

Same-key saves preserve the first received date; conflicting replacements fail.
Explicit deletion leaves a durable revision, and a save must present the current
revision to prevent a delayed operation from restoring a deleted key. Owner,
generation and vault-instance leases reject old results. Initialization is
serialized and file-locked. A zero-byte file reserves an outstanding master write;
after interruption, a missing master never authorizes another generated key.
The reservation is preserved for read-back. A reset/recovery interface for an
unresolved initialization has not been implemented.

The [vault tests](../packages/native/tests/native-chat-key-vault-test.cpp) passed
48 cases normally and under ASan/UBSan/LSan with leak detection and no suppressions.
They reproduced and corrected overlapping/concurrent master initialization,
replacement during a pending write and loss of an injected backend. They also
check restart, owner separation, deletion revisions, corruption/address swaps,
oversized records, symlinks, reentrant owner changes and future-version rejection.
These regular tests inject a synthetic credential backend. Caller-owned keys and
JSON/copy buffers are not a claim of complete memory erasure or hardware-backed
storage.

```sh
ctest --test-dir packages/native/build -R '^native-native-chat-key-vault$' --output-on-failure
```

The opt-in [platform test](../packages/native/tests/native-chat-key-vault-platform-test.cpp)
also passed against GNOME Keyring 50's real Secret Service on Ubuntu 26.04, using
a private session bus, fresh encrypted keyring and disposable synthetic owner.
Separate processes write and restore the key, mark it for sending, restore its
generation metadata and sending choice, delete it, restore its deletion
revision, write and restore a private request, complete a grant and restore its
key/receipts/removed request, then delete the granted key, remove the exact OS
master and confirm its absence. The bare
credential-adapter test passed against the same provider. The private provider,
bus and data were removed afterward. This is provider integration acceptance;
it does not establish the user's existing desktop keyring, other providers or
other operating systems.

To run deliberately against an available OS credential store, configure with
`-DWORKSPACE_NATIVE_PLATFORM_TESTS=ON`, then run:

```sh
ctest --test-dir packages/native/build -R '^native-chat-key-vault-platform$' --output-on-failure
```

## Durable requests and atomic completion

Pending requests keep the same local canonical message UUID, public/private
request pair, frozen device label, creation time and optional repair evidence
across restart. Binding to the public message placement permits an unbound or
legacy canonical binding to become the actual placement; conflicting rebinding
fails. A reused local intent cannot change its frozen wire fields.

Grant completion stores the generation key and completion receipts, then removes
the private request in the same SQLite transaction. Rejection completes only its
matching request and preserves any existing group key. Receipts are addressable
by both local request and public placement. Exact duplicate completion is a
no-op, including after later key deletion; conflicting outcomes/responses fail.
Generation deletion removes every pending request and repair-key copy for that
generation together with its revision tombstone. Old revision results cannot
restore them. Repair replacement additionally checks the saved prior group key
or an already-installed identical repaired key, preserving its received date.

This storage boundary does not authenticate a grant, check participant roles or
decrypt a repair probe. Those are requirements of the later provider/controller
workflow before it calls completion. Transaction regressions inject failure at
the second receipt write and during pending deletion, checking that keys,
receipts, private requests and deletion revisions roll back together.

Per generation, pending requests are bounded to 32 records and 8 MiB of encrypted
rows. Each encoded request is limited to 1 MiB and its repair envelope to 512 KiB;
larger inputs fail without eviction or partial changes. There is no secret TTL.
These native resource limits do not claim repair support for arbitrary 16 MiB
wire messages. The request implementation introduced schema version 2 to prevent
older binaries from ignoring the private-request deletion duties. The current
schema version 3 below also preserves sending metadata; the local `WSKV`
encryption framing remains version 1.

## Generation metadata and sending choice

The vault lists generation UUIDs and original received dates without returning
their private bytes. Listing sorts by received date descending and UUID ascending.
The last available key prefers the explicit per-stream sending order, then received
date and UUID. Receiving another generation or saving the same key preserves that
choice and the first received date. Repair completion preserves both metadata
fields. This is local preference, not proof of the server's current generation or
permission to send.

`markForSending` requires an existing key, the current owner/device lease and the
expected deletion revision. It changes only the encrypted sending order. It cannot
restore a deleted secret; deletion clears the order with the key. A failed write
or an exhausted safe integer counter leaves the previous choice unchanged. Runtime
operation/route guards and authoritative peer/stream checks remain the caller's
responsibility before using this storage API.

Schema version 3 accepts validated version-1/version-2 records, interpreting an
absent sending order as zero, without rewriting their ciphertext. Older native
binaries reject the new schema instead of discarding the metadata on later writes.
A forward-only SQL visitor decrypts one generation at a time under the existing
64 MiB database cap; listing retains only UUID/date metadata, and latest-key/order
operations retain constant private-key state. A regression reproduces an earlier
permanent count-cap dead end and verifies enumeration and selection across 4,097
encrypted generations, including deletion tombstones, without automatic eviction.
That separate test process peaked at 30,712 KiB RSS with no swap; this is not a
whole-application resource measurement or acceptance of complete encrypted history.

## Runtime and encrypted reading

The application opens the existing OS-backed key vault after verified project
login. Same-owner token refresh preserves its lease. Account/project changes,
logout, API ownership changes and material deletion invalidate stale work. An
explicit local-store retry is distinct from sending a chat-key request. The vault
is separate from the configurable message-cache budget, whose default is 10 GB
(10,000,000,000 bytes).

The controller prepares display data in bounded queued slices. Model getters do
not query the vault or perform HTTP. Current-chat access and an individual
message's historical generation have separate metadata; an available older key
can decrypt history while the current key is missing or null. Readable text
remains in scoped presentation memory and does not enter the response cache.
Encrypted Copy uses the native Markdown plain-text result. Ordinary message Copy
preserves its existing behavior.

Missing-generation requests retain their canonical UUID and wire data. A
GET-only status check resolves uncertainty; an explicit retry preserves the same
intent. Correlated grants authenticate stream, generation, devices, requester and
issuer against owned metadata before atomic vault completion. A pure archive
flag does not revoke read/request access or implicitly unarchive a chat. Newer
canonical placement evidence fences a moved request. Unknown unrelated request
IDs do not expand the observation window.

Local regression coverage includes source-generated grants, historical and
nullable generations, exact receipts, separate-process request restoration,
owner/API/material changes, archive intersections, realtime reconnect and bounded
500-row preparation. Earlier reader Runtime and TLS fixtures ran under ASan/UBSan/LSan;
the current issuer/order changes have not yet had that full sanitizer run.
Separate provider tests use a private GNOME Keyring instance; they do not establish
integration with the user's existing desktop keyring.

## Initial issuer workflow

The verified owner or administrator can grant the exact requested historical
generation or reject the request. A rejection does not require an X25519-sized
public key or local generation material. Granting requires the actual requested
key and a valid recipient pair; it never substitutes the current generation.
The response freezes its request, generation, sender, recipient and verified
default topic. Uncertain results expose GET-only Check and an explicit identical
Retry in the controller; complete user-facing recovery remains open.

The chat presents the source's centered request title and text actions in English
and Russian. Offered labels remain visible but disabled while sending. Only the
current device's own request hides inline issuer actions; this is a view rule,
not a controller authority restriction. An independent source recipient accepts
and decrypts the actual native grant, or records its rejection, in the UI fixture.

Public Grant/Reject rows use the source's exact sender/recipient/device labels,
24px service icons and inline Request action in English and Russian. Canonical
account UUIDs select the grammar even if the server ownership hint disagrees.
Unknown requests and other-device recipients keep their own unavailable/action-free
states. The original eleven public correlation/availability fields remain intact; QML also
receives the prepared service status and announcement summary. It never reads the
vault or encoded payload. Pending admission updates cached state immediately,
and guarded model notifications publish busy/completion/authority changes without
another history request. Actual regressions reproduce the stale Request after a
new canonical POST and the missing Request after fresh role restoration.

Earlier V31 matched 1440 by 900, DPR 1 source component scenes verified identical labels and
one 20px title line; their text bounding widths differ by less than 0.35px.
Three pre-fix rows reproduced an extra wrap caused by fractional width truncation.
Rounding the intrinsic width upward fixes them while the available-width cap
remains. Actual 700px Grant and rejected-Request flows additionally verify wrapped
text, containment and real clicks. Those earlier captures compare the response row. The later collapsed-group
comparison and its limits are recorded below; complete grouped-message parity
still requires the remaining interaction and resolution work.

Recipient restoration observes and binds a loaded request before shared authority
returns. A completed public placement cannot adopt a remaining legacy duplicate.
Regression tests reproduce both failures before the fixes, retain the original
move/deletion/restore controls, and positively check one shared authority request.
Complete key-management/recovery views and the remaining service-group workflows
still need acceptance.
An independent source runtime retains the first currently valid response. The native
controller now keeps bounded ordered public facts and recomputes cached outcomes
after full updates and fresh issuer authority. Actual regressions reproduce the
previous overwrite; both response orders, invalidation/restoration, role refresh,
move/deletion and initial history before Vault attachment/open now pass. Order is
seeded before display sorting without key access or reduction in history callbacks.
The combined observation cap remains 500 records and 2,000,000 UTF-16 units; existing
eight-row work slices and the 32-entry recipient queue are unchanged. Later Vault
replacement and genuine owner/route/access/API/cache/security loss still clear facts.

The request row centers each wrapped line and its multiline text. Four locale/theme
cases reproduce the earlier left alignment and pass after the local Flow transform.
Actual Grant and Reject clicks at the 700px minimum width also complete against the
independent source recipient. The source desktop permits a 700px window but keeps a
480px chat minimum: its unchanged component scene clips at the right edge there,
whereas the native chat compresses. This is a remaining global layout difference,
not evidence of identical narrow-window pixels.

## Service groups and history provenance

Ordinary/account filtering happens before date/topic/author grouping. Contiguous
service runs share a localized, keyboard-operable 28px header. A collapsed run
unmounts its bodies instead of keeping hidden crypto views alive. Initial accepted
history starts collapsed; eligible new live groups can open automatically, while
per-message manual state survives head deletion and split/merge. The offscreen
transition observes history/model/viewport barriers and does not override manual
expansion or the controlled focus/selection inputs. Product entrypoints for every
selection/focus mode remain incomplete.

The controller prepares announcement and required/waiting/unavailable summaries
in the existing eight-row work slices. The combined 500-record/2,000,000-UTF-16-unit
observation bounds remain. Two actual regressions cover status publication when
the fact limit is reached and direct request admission at that limit. Unknown
out-of-window request bodies now have the owner-scoped demand path below.
Ordinary/account public preclassification is described below; ordinary/provider
key issuance remains unavailable.

The existing cache schema4 adds an owner+UUID Body key alongside History pages;
older binaries reject these semantics rather than treating them as ordinary pages.
It retains the same worker, cache budget and owner lease, with no second body store.
An exact Body miss tries at most four indexed History pages; legacy pages lacking
verified viewer identity cannot supply an owned body. Returned rows are allowlisted
public snapshots, not complete transport DTOs or proof of current access. They do
not insert R into visible history, and never contain decrypted text or private keys.
Required removal covers the exact Body and its indexed History copies; partial
message updates preserve fields not supplied by the event.

In an encrypted stream, only a genuinely mounted Grant/Reject body demands its
typed linked request. Collapsed encrypted groups, model getters and history
ingestion make no lookup. Ordinary/account preclassification has the separate
loaded-row path below.
The controller coalesces equal R across consumers, including when the first
response disappears. Exact cache/History fallback can publish useful recipient
facts before the single project+UUID GET refresh completes; transport errors keep
that fallback. Four active operations share the existing 500-fact/2,000,000-unit
bounds. Owner/API/route/cache/security changes and newer R revisions fence late
results. Vault/runtime reset reasserts only still-mounted demand.

For encrypted body demand, routine re-render or delegate reconstruction does not
retry a settled miss. Genuine collapse/reopen uses a new disclosure serial and may
retry transport failure or snapshot omission; a typed body-present negative remains sealed.
After omission or an observed realtime deletion, a new disclosure captures the
current revision and requires fresh canonical HTTP before restoring R. Old cache
or replies cannot resurrect it. Selection-mode remount has a controlled reducer
regression; the complete physical selection/focus entrypoints remain open.

Accepted history has an explicit commit revision/readiness boundary. Realtime
metadata does not impersonate a newly accepted history window. Actual HTTP
regression coverage preserves the last visible message's original snapshot rank
when timestamps differ below display precision; display order is unchanged.

Matched English/dark two-event and Russian/dark five-event collapsed component
scenes use identical ordinary message text/placement at 1440 by 900, DPR 1 and the
same Moscow timezone. The visible service header has exact y/height and less than
1px width difference. The Russian comparison shows the same 16px gap before the
next ordinary message; the previous 12px gap failed the same regression before the
local spacing fix. A final run without a following message retains its previous
height. That last-height oracle is derived from the source CSS, not a separately
captured terminal source scene. Full sidebar/footer/profile inputs and expanded
crypto bodies are outside these two matched scenes.

The unchanged pinned Source grouping suites passed 221 checks; separate
resolver/API/bubble suites passed 109 checks. They are functional reference tests,
not native or live acceptance. Local V33 coverage passes 231 encrypted Core,
83 cache cases covering Body and 53 cache-service cases, plus 15 focused mounted-disclosure UI
cases. These cover actual TLS lookup, worker readback, shared-R retirement,
owner/runtime barriers, useful fallback and genuine-remount retry. The complete encrypted UI121, general Controller739 and Window220 groups also
pass on the same coherent inputs, along with Runtime10, locale14 and Markdown21.
Run the current cache, service, TLS and visible UI groups from the configured build:

```sh
ctest --test-dir packages/native/build \
  -R '^native-native-(cache|cache-service|e2ee-read|e2ee-ui)$' --output-on-failure
```

Matched Source/native expanded Grant and Reject scenes at 1440 by 900, DPR 1 use the
same canonical public R/G/J inputs. The unchanged Source components exercise their
actual quote hook, collection GET, body correlation and IndexedDB cache. Their safe
key/permission state is a synthetic component adapter, not Source crypto-runtime,
live-service or full-shell acceptance. Remaining interaction entrypoints and
ordinary/provider key issuance remain open.

V34 also prepares public recipient facts for ordinary/account feeds before
relevance filtering and grouping. A loaded foreign Grant/Reject can therefore
become relevant to its actual requesting account even without an attached Vault,
including in an external/provider stream. An out-of-window request remains a body
supplement, without insertion into visible history. Current-account authors remain
relevant while their public recipient labels resolve; ownership hints do not replace canonical account UUIDs.

This queued pass reuses the existing Body/indexed-History resolver, eight-row work
slices, four active lookups and combined 500-record/2,000,000-UTF-16-unit bounds.
Same-R consumers coalesce without getter I/O. A settled transport failure or
omission does not retry on routine updates or another same-R response; retirement
of all references followed by a new consumer permits a fresh attempt. An unfinished
lookup canceled by a real WSS trust interruption resumes after verified catch-up
without inventing a new disclosure. That liveness correction does not reopen a
settled failure. Owner/API/route/cache/access barriers still fence old results.

Public correlation without a Vault creates no pending request, current-device or
key-availability fact, or issuer authority. It enables no ordinary/provider key
request, grant, rejection or encrypted send; existing plain-message policy is
unchanged.

Three actual Main rows cover foreign Grant/Reject in native and provider feeds
without a Vault: acknowledged R GET while the list has no groups or bodies,
automatic recipient publication, ready-empty auto-opening, then one real Space
collapse with no second GET or POST. Two English/light Source37 comparisons use
identical canonical public DTOs, a null crypto session and the actual Source
account resolver, global body store, IndexedDB and keyboard collapse. They do not
manufacture key state or accept full shell, unknown participant counts, ordinary
issuance or a live account.

The V34 Core suite passes 246 cases and the complete encrypted UI group passes
124 cases. Three focused Main rows and two capture rows also pass, producing five
and four Qt results including fixture setup/cleanup. The complete Controller739, Window221, Runtime10, locale14 and Markdown21
groups also pass on the current inputs. Cache83 and Service53 retain byte-exact
applicable production and test inputs. The staged package passes installed runtime
checks and anonymous visible Wayland startup, single-instance activation and quit.

## Window and package acceptance boundaries

The encrypted window uses the pinned desktop components as its visual reference.
Synthetic English/light and Russian/dark captures compare missing access and
readable history at 1440 by 900, DPR 1. Header geometry, locked placeholder paint,
author/time placement, avatar circles, encrypted sidebar locks and duplicate
access notices have native Qt regression coverage. The source reference is a component scene rather than the complete
live ChatPage. The key-update notice now has source-colored text and primary
actions on the composer surface, with intrinsic inline/wrapped layout. Four
locale/theme paint checks and four geometry checks cover this state; narrow
layouts, generic error/recovery notices and the complete joined composer still
need matched visual acceptance. Participant-count and remaining composition gaps
must not be presented as complete pixel parity.

The current local V34 coverage is listed above. All C++ production and MockServer
inputs are byte-exact to the accepted Core246 run, as is effective non-UI
test code. Cache/service applicable runtime and test inputs match their
accepted groups. Complete UI/controller/window groups use the current source.
The locale validator checks every nested plural form and retains placeholder
multiplicity. Getters expose cached sending-key/service state without vault or
HTTP work. Earlier preparation/event-loop figures are not fresh current-application
performance acceptance.
Historical selection, nullable/current keys, closed topics, key deletion and
route changes have separate controls; these groups do not establish every
encrypted workflow.

Earlier Check/Retry input failures remain recorded. The current harness waits
for a rendered action and a real click receipt before checking its result;
observed 4px geometry changes did not prove the original missed-click cause.
The current V34 general Window group passes 221 cases without Qt warnings. Scoped
focus-aware teardown clears active input before surface destruction; real-window
regressions cover focus loss while the surface is still alive and engine teardown.
These checks do not establish the cause of earlier missed-click observations.

The current V34 staged Linux package passes the installed dependency/TLS/crypto
probe, anonymous visible Wayland startup, single-instance activation and Ctrl+Q exit 0.
All 46 inspected runtime libraries map inside the package. Temporary settings,
data and cache directories and both processes were removed. The preview app ID
remains unregistered with the host desktop portal. Authenticated packaged workflows,
live service acceptance, complete Linux parity and other operating systems remain
open.

Durable outgoing messages, encrypted attachments/edit/reply/forward,
bootstrap/rotation, full issuer recovery and key-management UI, and cross-device/live service
acceptance remain open. The native interface must continue to expose unsupported
states without a plaintext fallback.

A controlled teardown regression first observed one asynchronous creation at the
first Hide event with the old helper. The scoped test helper now observes a real
visible frame and zero public incubators before the existing focus-aware close,
then zero after close. All ten scroll rows retain their original assertions, and
CI fails on the exact pending-creation warning. The original whole-run warning
about two private creations did not reproduce in an isolated instrumented scroll
run; its precise cause remains unknown. This is a test resource-boundary fix, not
a production rendering or timing change.


## Current ordinary participant responses

The current branch extends ordinary native responses from managers to current
participants already holding the exact requested generation. Fresh self/requester
membership and current native ownership are required. Manager Reject remains
independent of local A. Encrypted and global issuer permissions are unchanged;
ordinary key acquisition/bootstrap and provider issuance remain open.

Actual Source-recipient and Main-click regressions cover Grant/Reject. Two
separate actual loss-of-A regressions cover retained Reject publication and the
acknowledged request GET during Retry. The same current intent now settles on
lost write admission; no extra POST, new target or UUID is introduced. Reject
still permits public status Check without A and explicit same-wire recovery
when current eligibility is restored. Grant's deletion/material fence remains.

The holder snapshot's retained issuer checks covered the controller. The later
Main integration reuses the same eligible Grant/Reject action for a matching
retryable saved response: an explicit click enters the existing GET-first,
same-wire recovery. Actual applied/unapplied 503 flows pass for both actions,
including Source-recipient completion; another R and the opposite action cannot
borrow the saved permission. No separate Check/Retry controls were added.
Permanent Grant material loss, status-only recovery, switching action/request and
Source failure-toast presentation remain open; see
[Main recovery checks](provenance/same-action-main-recovery-checks.json). Current complete groups and preserved failures are recorded
in [holder checks](provenance/ordinary-holder-checks.json). The configurable disk
cache still defaults to 10 GB. Full Linux, live service, packaging and other
platforms are not established by these local synthetic checks.

## Candidate: canonical encrypted Direct from New chat

The existing Direct lookup now admits verified active encrypted pairs with a
valid or null current generation. Pair/viewer/project, native source, archive,
access, selection and canonical default/catalog guards remain. No encrypted-pair
creation, bootstrap, group-key import or plaintext downgrade is introduced; the
null-current/no-key case stays locked.

One actual Main/TLS case physically opens that Direct, selects its topic and
requests A. Its typed POST echo binds localId/public UUIDv5/default topic in the
Vault without another R GET. The unchanged Source peer grants to that exact
request, Native reads the sample, and physical Send produces e2ee-only wire
confirmed by the existing canonical GET and decrypted by the same peer. No A or
private pair crosses the pipe. New Main (3 Qt cases) and old peer/composer controls (3/8 Qt cases) pass without
runtime warnings, stderr or skips; Controller (837 cases) passed separately.

This uses synthetic HTTP and public event admission, not live WSS/backend or a
full Source UI session. The peer reconstructs typed public request input; full
transport-DTO parsing and pixel parity are not claimed. The whole current E2EE core passed 274 cases. Whole Window285 and encrypted UI141 passed. Public package delivery and the anonymous installed 0.1.4 to 0.1.5 upgrade also
passed; live account, authenticated history and credential preservation remain open. Distinct earlier
failures and causal limits are retained in [candidate checks](provenance/linux-0.1.5-candidate-checks.json).
