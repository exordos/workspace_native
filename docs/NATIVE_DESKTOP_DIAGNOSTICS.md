# Native diagnostics: rotating log file and memory snapshot providers

Issue: #77 (part of #49 — shell-side providers for the future Diagnostics
pages #50/#51/#52; the pages themselves are out of scope here). Reference
contract measured from `workspace_ui` at sha
`61dacef78dab2f5d0be76f283bebf49fe29b84af`.

## Reference contract (measured)

- `packages/electron/src/main.ts:112-116` — `LOG_DIR_NAME = "workspace_logs"`
  under `app.getPath("userData")`; files `workspace.log` (`LOG_FILE_NAME`) and
  rotated `workspace.log.1` (`LOG_ROTATED_FILE_NAME`);
  `MAX_LOG_FILE_BYTES = 1048576` (1 MiB); `MAX_LOG_LINE_LENGTH = 32768`.
- `main.ts:146-160 rotateLogsIfNeeded(filePath)` — when the current log file
  exists and its size is `>= MAX_LOG_FILE_BYTES`: delete the previous
  `workspace.log.1` if present, then rename `workspace.log` to
  `workspace.log.1`. Rotation is best-effort (errors swallowed, append
  proceeds).
- `main.ts:162-182 appendLogsLine(rawLine)` — sanitize first: strip control
  characters matching `[\u0000-\u0008\u000b\u000c\u000e-\u001f\u007f]` (tab
  `\u0009`, LF `\u000a` and CR `\u000d` survive), then `slice(0, 32768)`, then
  `trim()`; an empty result writes nothing and returns `false`. Then ensure the
  log directory, rotate if needed, and append
  `${new Date().toISOString()} ${sanitizedLine}\n` — an ISO-8601 UTC timestamp
  with milliseconds (`yyyy-MM-ddTHH:mm:ss.zzzZ`), a space, the line, and a
  terminating newline. Returns `false` on any I/O error.
- `main.ts:1228-1235 logs:getFilePath` — ensures the directory and returns the
  absolute `workspace.log` path (`null` on failure).
- `main.ts:1237-1240 logs:append` — forwards non-string payloads to `false`.
- `main.ts:1242-1267 diagnostics:getMemorySnapshot` — returns
  `{ collectedAt: <ISO>, main: process.memoryUsage() (bytes: rss, heapTotal,
  heapUsed, external, arrayBuffers), system: process.getSystemMemoryInfo()
  (kB: total, free, available, swapTotal, swapFree), processes:
  app.getAppMetrics() mapped to { pid, type, memory: { workingSetSize,
  peakWorkingSetSize } (kB), cpu: { percentCPUUsage } }, totalWorkingSetKb:
  sum of workingSetSize }`.
- The React side (`packages/web/src/shared/lib/logger.ts`) mirrors every
  `createLogger()` line into `logs:append`, so the file accumulates one
  timestamped line per logger call.

## Native design

New module `packages/native/src/native-logging.{h,cpp}` inside
`workspace-native-core` (class `workspace::NativeLogging`, QObject). All file
I/O is synchronous best-effort like the reference: failures never throw and
never abort the app, they return `false`/empty.

- Paths. Root defaults to `QStandardPaths::writableLocation(AppDataLocation)`
  (issue contract; same precedent as the chat key vault and session vault).
  Constructor takes an injectable root directory so tests never touch
  developer paths: `NativeLogging(QString rootDir = {}, QObject *parent)`.
  `logDirectoryPath()` = `<root>/workspace_logs`,
  `filePath()` = `<root>/workspace_logs/workspace.log`,
  `rotatedFilePath()` = `.../workspace.log.1`. The QML-facing
  `getFilePath()` (Q_INVOKABLE) mirrors `logs:getFilePath`: it ensures the
  directory and returns the path (empty string on failure, since QML has no
  `null` convention here).
- `appendLogLine(QString)` mirrors `appendLogsLine` byte-for-byte: same
  control-character class, same 32768-char slice followed by trim (slice
  before trim, like the reference), skip-empty, ensure directory, rotate, then
  append `ISO + ' ' + line + '\n'`. ISO timestamp from
  `QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs) + 'Z'`, which
  matches JavaScript `toISOString()` exactly (`yyyy-MM-ddTHH:mm:ss.zzzZ`).
  Rotation: if the current file size is `>= 1048576` before the append, delete
  `workspace.log.1` if it exists and rename the current file to it. A file of
  exactly 1 MiB rotates on the next append; 1 MiB − 1 byte does not.
- Thread safety: Qt logging runs on whatever thread emits the message, so the
  directory-create + rotate + append sequence is serialized under a mutex
  (deviation: Node is single-threaded and needs none).
- `installNativeLogHandler(NativeLogging &)` installs Qt's process-wide
  message handler. The developer console receives the formatted Qt message.
  The rotating file records its timestamp, severity and a fixed diagnostic
  class for network, QML, platform or other Qt messages. Two reviewed
  application messages retain their static wording for a missing direct-chat
  topic or a failed restart. The disk writer discards arbitrary message text,
  including file paths, URLs and identifiers. A failed console write still
  allows the file append to complete.
  Handler and install/remove synchronize on one process-wide recursive
  `QReadWriteLock`: the handler holds the read side while it consults the sink,
  and `removeNativeLogHandler()` restores the default handler first, then
  takes the write side — which only returns once in-flight handler calls on
  other threads have drained — before clearing the sink pointer, so callers
  may destroy the sink immediately after (fix round 1, thread 4575150370).
  The recursive mode lets a nested log from a thread already inside the
  handler re-enter without self-deadlock. `formatMessageLine(QtMsgType,
  message)` is exposed for tests.
- `memorySnapshot()` (Q_INVOKABLE, returns `QVariantMap`) mirrors
  `diagnostics:getMemorySnapshot` with portable Linux procfs sources; the proc
  root is injectable for tests (default `/proc`):
  `{ collectedAt: <ISO>, main: { rss, size } (bytes, from
  /proc/self/status VmRSS/VmSize), system: { total, free, available } (kB,
  from /proc/meminfo MemTotal/MemFree/MemAvailable), processes: [ { pid,
  type: "Main", memory: { workingSetSize, peakWorkingSetSize } } (kB, VmRSS/
  VmHWM) ], totalWorkingSetKb }`. Missing inputs degrade to `0` values so the
  key set is stable.

## Deviations from the reference (recorded)

- Single native process. Electron reports one entry per GPU/renderer/utility
  process; the Qt client is a single process, so `processes` always holds
  exactly one `type: "Main"` entry and `totalWorkingSetKb` is that process's
  working set. No per-GPU-process list exists to port.
- `main` keeps the reference key name but only reports `rss`/`size`: V8
  heap breakdowns (heapTotal/heapUsed/external) have no /proc equivalent.
  `rss`/`workingSetSize` are the portable RSS figures.
- Platform portability note: procfs sources exist on Linux only, and
  `memorySnapshot()` reads them solely under `Q_OS_LINUX`; on other platforms
  it returns the full key set with zero values and performs no `/proc` access
  at all (fix round 1, thread 4575150368; a future task can map `mach`/`psapi`
  sources). Linux is the acceptance target, so procfs is authoritative there.
- The rotated-file and line-length budgets, names, sanitize class and ISO
  prefix are reproduced 1:1 (parity-preserving per the issue).

## Wiring and scope

- `src/main.cpp` constructs a `NativeLogging` before first use and installs
  the message handler. Ordinary `qInfo`/`qWarning` traffic produces a classified
  event in `workspace.log` with its original severity.
- No UI and no `tr()` strings: pages #49/#50/#51/#52 consume these providers
  later; this issue is provider-only (client-side files + local process stats,
  no network/sync/UI behavior).
- No QML context property yet — the provider is QML-ready (Q_INVOKABLE,
  `QVariantMap`/`QString` types) and gets registered when the pages land.

## Tests

Extended in the existing `native-native-controller` group (issue rule: no new
CTest groups): path layout under an injected root, ISO-8601 line prefix and
newline termination, empty-after-sanitize rejection, control-char stripping
(0x07/0x1B), 32768-char truncation, rotation at exactly 1 MiB (and not at
1 MiB − 1), rotated file overwritten not duplicated (no `workspace.log.2`),
snapshot key set and values from fixture proc files, real-/proc sanity on
Linux, and the message-handler line formatting. The fix-round-1 cases
(PR #289) add: a stderr-write failure still appends the line to the file; the
provider's `Q_OS_LINUX` procfs gate is source-guarded and the procfs-positive
asserts (fixture values and real-/proc) stay gated while the key set and the
missing-source zero-degradation run on every platform; and `removeNativeLogHandler()`
drains an in-flight handler call parked in another thread before returning
(the worker parks deterministically on a full stderr pipe). The fix-round-2
cases (PR #289) add: a source guard asserting every POSIX-only include and
raw descriptor call in the test translation unit sits inside a
`Q_OS_UNIX`/`Q_OS_LINUX` guard (with portable wrappers in
`tests/msvc-compat.h`) so `BUILD_TESTING` compiles on Windows; the two
descriptor-based message-handler cases (stderr-failure, drain-on-removal)
compile under `Q_OS_UNIX`; and the drain case is pipe-capacity independent —
its reader starts before any write can block, the filler fills the pipe with
`O_NONBLOCK` writes probed until `EAGAIN` (no 64 KiB assumption), and on
Linux the pipe is pinned to its 4 KiB minimum (`F_SETPIPE_SZ`) so the
small-capacity regression path is always exercised. Every exit path of the
drain case (success or a failed assertion before the removal was requested)
must set the cleanup flag so the reader releases and drains before the
releaser and worker joins — otherwise an early failure joins a releaser still
waiting for `removalRequested` and hangs the case forever (PR #289,
thread 4575792864).

The Linux keyboard check opens Diagnostics from the account menu and reaches
View logs with real Tab input. A sidebar avatar and its topic expander each
take a Tab stop. The check follows the actual focus path until it reaches the
header or repeats a control. It then checks the exact order of the refresh and
copy actions, keyboard activation, the accessible press action and modal export.
