# Native disk-cache measurements

The default global limit is **10 GB = 10,000,000,000 bytes**, configurable in
settings. These Linux/Qt 6.10.2 measurements distinguish cache-component costs
from complete application costs. They do not establish full functional parity,
live-server savings or clean-machine acceptance.

## Synthetic fill and reopen

An earlier synchronous storage snapshot received 11,000 history pages, each
containing 50 messages with roughly 20,000 characters per message. Physical
database, WAL and SHM size was checked against the default limit after each put.
The workload intentionally exceeds retention capacity.

| Measurement | Observed result |
| --- | --- |
| Final physical size | 9,965,379,584 bytes, below 10 GB |
| Retained history pages | 9,587 |
| In-memory freshness bound | At most 4,096 keys |
| Fill duration | 1,826.498 seconds |
| Per-put median / p95 / maximum | 126.660 ms / 414.374 ms / 1.257 s |
| Component peak RSS | 24,588 KiB |
| Reopen / last-page read | 92 / 12.351 ms |

The reopen measurement retained the operating system's filesystem cache. It is
not a cold physical-disk read. The original calling-thread shrink blocked, which
led to the serial asynchronous cache worker and cooperative cancellation.

## Large physical reclamation

The subsequent asynchronous run reused the previously filled and partially
trimmed database. It began with **917 retained pages** and 9,124,605,952 physical
bytes; it did not repeat a fresh 10 GB LRU eviction workload. An initial reduction
was cancelled and safely reopened, followed by the full physical reduction.

| Measurement | Observed result |
| --- | --- |
| Physical size after cancellation / reopen | 9,107,808,256 bytes |
| Applied new limit | 1,000,000,000 bytes |
| Final physical size | 966,443,008 bytes |
| Retained pages | 917; the newest 50-message page was read after shrinking |
| Full physical-reclamation duration | 1,906.420 seconds, about 31.8 minutes |
| Maximum gap of the independent 5 ms event-loop timer | 25.685 ms |
| Component peak RSS | About 24 MiB, including an external process-status sample |
| Final cooperative shutdown | 3 ms |
| CPU / swap | About 98% of one core; no swap |

The helper reported success and `/usr/bin/time` recorded child exit status 0.
The outer execution session separately returned 143; its cause was not verified.
Independent checks confirmed that the helper process was gone and its temporary
database directory had been removed. The results establish retained data,
physical reclamation, event-loop responsiveness and cleanup. **The duration and
CPU cost remain a performance gap.** This is not evidence that a large shrink
is fast or that the whole application uses 24 MiB.

For comparison of measurement scope, the later V13 packaged anonymous sign-in
process had a sampled RSS of 224,112 KiB, about **219 MiB**, with 17 threads on
this desktop. That includes the Quick presentation and runtime libraries, rather
than just the cache helper. It is neither a complete messenger workload nor a
controlled comparison against Electron; whole-application resource acceptance
remains open.

## Bounded vacuum regression

Qt's QSQLITE driver treats zero-column `PRAGMA incremental_vacuum(N)` results as
non-select statements. Calling `next()` does not drain their repeated SQLite
steps. The cache therefore uses cancellation-aware single-page steps inside
bounded transactions and checkpoints once per transaction, without borrowing a
native SQLite handle or linking a second SQLite implementation.

The physical-resize regression measured **2,488 checkpoints before the fix and
22 after it** on its scaled fixture. Its assertion bounds checkpoint count,
preserves the newest page, checks final physical usage and reopens the database.
A second regression cancels during vacuum, reopens and completes the reduction.
These resource/integrity assertions do not depend on a wall-clock speed threshold.

## Large response bodies and identity lookups

A later regression filled twenty roughly 1 MiB history pages and requested 64
absent page markers. Linux `/proc/self/io` reported **303,931,510 read characters**
with the original `entries WITHOUT ROWID` layout, and **114** after moving new
response entries to an ordinary rowid table. The latter count includes reading
the accounting file itself. This measures per-process read calls, including data
already cached by the kernel; it is not physical disk traffic or a wall-clock
threshold. The meaningful invariant is that negative lookups must not read large
unrelated message bodies. This regression is Linux-specific; other operating
systems still require their own resource acceptance.

New cache files keep response BLOBs in rowid leaves and the composite owner/key
identity in a compact primary-key index. Small message-membership rows retain
their existing layout. This follows the [SQLite guidance for large rows](https://www.sqlite.org/withoutrowid.html#when_to_use_without_rowid)
and adds no storage service or SQLite engine. The SQLite page cache remains
**2 MiB**. These layout measurements used logical schema version 3 with the same
owner/TTL/lease/payload/physical-budget contracts. The current schema 4 also stores
owned message Body supplements within the same worker and physical budget; these
earlier layout measurements do not establish its complete application cost. Existing `WITHOUT ROWID` files remain readable, writable
and shrinkable, with a dedicated restart/physical-bound regression; they are not
automatically rebuilt, and their large-BLOB performance is not claimed fixed.

The updated storage suite passed **54/54** cases normally and under
ASan/UBSan/LSan. Its unchanged worker service passed **44/44** against the new
layout in both runs, without sanitizer suppressions or diagnostics.

A public-path comparison wrote 2,000 unique 50-message pages, each roughly 1 MiB,
then reopened and reduced the limit to 1 GB. Both layouts retained 927 pages,
ended at 966,451,280 physical bytes and read the newest page after reduction and
restart; both temporary databases were removed.

| Component measurement | Original layout | New rowid layout |
| --- | --- | --- |
| Fill | 303.616 s | 35.296 s |
| Per-put median / p95 | 140.236 / 268.265 ms | 14.952 / 32.028 ms |
| Latest-page read after reopen | 502.167 ms | 6.042 ms |
| Reduction to 1 GB | 211.649 s | 24.873 s |
| Peak RSS | 24,608 KiB | 23,876 KiB |

These are single observations on a developer machine with other concurrent task
workloads, not an isolated speed guarantee or complete application comparison.
The later full worker run wrote 11,000 pages through `NativeCacheService`,
checked the physical limit after each completion and retained 9,589 pages at
9,965,379,584 bytes. Fill took 278.035 seconds; per-put median/p95/maximum were
20.905 ms / 47.156 ms / 744.815 ms. Cold reopen took 71 ms. A reduction cancelled
after 500 ms stopped in 2 ms, safely reopened and preserved the newest page.

**The complete reduction to 1 GB failed the helper's 30-minute whole-run
watchdog.** A read-only inspection after exit found 917 retained pages and the
exact newest 50 messages, but physical usage was still 3,349,184,512 bytes. This
does not establish successful final reclamation. The worker used at most
30,796 KiB RSS. The maximum gap of its independent
5 ms timer reached 650.485 ms while other task builds and large synthetic
filesystem experiments ran; this is not isolated UI-responsiveness acceptance.
The child and wrapper exited with status 1, all owned processes were reaped and
the exact synthetic cache directory was removed. The source did not change during
the run. The large-maintenance performance gap therefore remains open despite
the passing storage and service suites.

A regular frozen test also passed under `strace`. An initial attempt to trace
the sanitizer binary failed because LeakSanitizer does not support ptrace;
that instrumentation failure is not a product leak. The corrected regular run
recorded syscall counts separately from the untraced ASan/UBSan/LSan checks.

## Maintenance experiments

### Reclaim while evicting

A subsequently adopted implementation keeps the existing 4 KiB file layout and 2 MiB SQLite
page cache. During budget reduction it temporarily selects FULL auto-vacuum
before bounded LRU deletions, then restores INCREMENTAL. This prevents each
deletion's free pages from accumulating into a whole-cache freelist. The first
automatic compaction is admitted only when the existing freelist fits within one
permitted response's size. Already large freelists retain the existing
cancellation-aware incremental path instead of an unbounded automatic commit.
The operation remains on the serial cache worker; no engine or service was added.

Its scaled regression measured 9,240,576 accumulated free bytes before the fix and
zero after it, without a wall-clock threshold. Separate cancellation after an
actual deletion preserved the newest page, reopened the database and completed
the smaller limit. The candidate passed 56 storage and 44 service cases normally
and under ASan/UBSan/LSan, without suppressions or diagnostics. This isolated
closure precedes the additional direct-chat private-metadata regression.

The public 2,000-page workload filled in 24.219 s and shrank in 8.337 s, retaining
928 pages at 965,488,720 bytes, with 23,656 KiB peak RSS. A subsequent complete
worker run exercised default-budget overflow, cancellation/reopen and final
reclamation:

| Component measurement | Observed result |
| --- | --- |
| Fill / retained pages | 238.000 s / 9,589 pages |
| Physical size at the default limit | 9,965,379,584 bytes |
| Per-put median / p95 / maximum | 19.130 / 38.394 / 211.231 ms |
| Cold reopen | 172 ms |
| Cancel after 500 ms / cooperative stop | 7 ms |
| Reopen after cancellation | 65 ms |
| Completed reduction to 1 GB | 137.177 s |
| Final physical size / retained pages | 966,352,896 bytes / 928 pages |
| Sampled peak physical size during resumed reduction | 9,924,341,840 bytes, within the old applied 10 GB limit |
| Whole-helper peak RSS | 30,536 KiB |
| Maximum gap of its independent 5 ms timer | 18.018 ms |
| Whole workload / final cooperative stop | 376.030 s / 2 ms |

The exact newest 50-message page survived cancellation, both reopens and completed
reduction. The helper and outer runner exited with status 0; temporary data and
owned processes were removed. These are developer-machine observations with other
task work, not an isolated speed guarantee, complete application measurement or
live-network acceptance. The 10 GB helper used the frozen predecessor Domain
without direct-chat private metadata. Its 1 GB target has the same 8 MiB initial
freelist guard as the later `entryLimitBytes()` implementation; the latter also
scales down the guard for small configured budgets. Shared integration with that
optional metadata then passed **57/57 storage and 44/44 service** cases normally
and under ASan/UBSan/LSan, with zero diagnostics and unchanged frozen inputs. Those
two verified cache files were copied to the shared native source; final whole
application integration remains separate. Existing large-freelist performance is
not claimed fixed.

`/usr/bin/time` reported zero swaps, but its `getrusage().ru_nswap` field is
unmaintained on Linux and does not establish absence of swap usage. The helper's
swap usage was not independently sampled; do not turn that counter into swap
acceptance or a whole-computer resource claim.

### Rejected buffer and page-size candidates

An isolated 5.26 GB raw SQLite file with roughly 4 GB of free pages tested the
large-freelist hypothesis separately from the public cache workflow. The same
10,240 single-page vacuum steps (40 MiB, in 256-step transactions with checkpoints)
read 31,867,084,908 characters with a 2 MiB SQLite page cache and 130,244,711 with
16 MiB. Elapsed time was 13.449 versus 11.875 seconds, while peak component RSS
increased from 15,792 to 30,828 KiB. The read-call reduction did not establish a
compelling elapsed-time improvement, so the larger buffer was not adopted.

A separate 64 KiB-page fixture reclaimed the same 40 MiB in 640 steps, taking
3.233 seconds with 164,701,978 read characters and 15,872 KiB peak RSS. This is
an exploratory raw-SQLite observation, not a production cache change, complete
shrink or normal-navigation acceptance. Larger pages also change ordinary I/O,
small-budget capacity and legacy-file compatibility; those tradeoffs must be
tested before adoption. An initial probe-only partial-batch counting error was
preserved and corrected before this sample. All experiment files and processes
were removed. These observations ran alongside other development workloads.

A following public-path 2,000-page comparison demonstrated why the raw vacuum
sample is insufficient. Ordinary 4 KiB pages filled in 24.150 s and shrank in
21.021 s, retaining 927 pages at 966,451,280 bytes. The 64 KiB candidate filled in
35.568 s and shrank in 12.414 s, retaining 902 pages at 966,557,776 bytes. Latest
page reads after reopen were 4.296 and 4.913 ms; component peak RSS stayed near
23 MiB. Both preserved the newest page and removed their temporary files. The
larger-page candidate worsened this fill observation and retention density, and
was not adopted. Both use the predecessor Domain without the later direct-chat
private-field extension; this is a cache-layout experiment, not DM acceptance.

Isolated candidates have not changed the production cache. On a 266 MB fixture
with one message per page, increasing the transaction cap reduced checkpoints
from 208 to 28, but physical shrink stayed at about 1.7 seconds. Reusing a
prepared vacuum statement also showed no measured improvement. Those results
do not justify a larger production transaction or longer cancellation boundary.

A second fixture used 2,000 pages of 50 messages, about 340 MB including indices,
on the task's ext4 filesystem. Each variant preserved the newest page, met the
64 MiB final limit and reopened successfully:

| Candidate | Shrink duration | Whole-helper peak RSS |
| --- | --- | --- |
| Existing 2 MiB SQLite page cache | 16.205 s | 19,552 KiB |
| 16 MiB SQLite page cache | 14.012 s | 34,348 KiB |
| Existing page cache, spill disabled | 17.217 s | See retained run log |

These are single exploratory runs alongside other development work, not a
controlled performance comparison. Increasing memory did not establish a
compelling benefit, and disabling spill did not improve this observation. Neither
candidate was adopted. This smaller workload does not replace the large physical
reclamation measurement or resolve its performance gap. All synthetic databases
and worker processes from these experiments were removed after each run.

An instrumented repeat of the 340 MB fixture separated 13.898 seconds of LRU
eviction from 2.237 seconds of physical reclamation; 0.793 seconds of the latter
was checkpointing. A diagnostic transaction around 64 evictions increased those
phases to 19.361 and 1.793 seconds respectively, with 0.920 seconds spent in vacuum
checkpoints. That transaction candidate was not adopted: it did not establish a
speed benefit, and its cancellation/rollback behavior was not acceptance-tested.
The phase split explains this eviction workload only; it does not diagnose the
separate 9 GB run, whose obsolete entries had already been removed.

## Remaining acceptance

- Complete whole-application acceptance with the current projection, controller,
  UI and cache implementation; repeat the full workload when changes affect its
  write shapes, ownership or physical-reclamation path.
- Improve the pre-existing large-freelist fallback's CPU and elapsed time while
  retaining cancellation, bounded memory, the physical limit and newest-page integrity.
- Measure the complete packaged messenger, repeated-navigation REST traffic,
  idle CPU, reconnect and real-network bytes against the reference desktop.
- Verify actual offline restoration, account removal, installation and OS
  credential-store behavior on the intended Linux targets.
