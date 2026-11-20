# Local support reports and native crash collection

**Status** Adopted when this decision lands on the default branch.

**Decision** Start with a user-reviewed local JSON report and defer native
crash dumps and remote delivery. The report has a schema version, an independent
random report ID, build and runtime information and optional aggregate event
counts. Cap the whole file at 64 KiB. Snapshot at most the existing 500 session
entries. Keep one immutable preview per open reporting form, discard it on
cancellation or owner change, and save the reviewed bytes atomically. The file
selected by the user is the only persistent report copy. The local exporter
has no network, symbol-server or screenshot dependency.

| Option | Diagnostic value | Packaging, data and operating cost |
| --- | --- | --- |
| Reviewed local JSON | Gives support a build identity, environment, bounded counts and a reproducible narrative | Existing Qt runtime, preview works offline, no collection service |
| Sentry Native | Can associate native crash events with debugging information on Linux, macOS and Windows | Adds a CMake SDK, backend-specific binaries, symbol upload and private server or hosted service. Event filtering alone cannot guarantee dump privacy on every backend. |
| Crashpad | A separate handler can capture process state when the failing client cannot run normal code | Requires a packaged handler, local dump storage and, for centralized analysis, a collector with symbol access and defined retention. Captured memory may contain decrypted content. |

A clean-shutdown marker, a stalled window and a captured crash describe different
events. The local report cannot provide a postmortem stack. Revisit dump
collection only with an accepted private destination, access and retention
policy, explicit consent, symbol ownership, licensing and operating costs, and
installed tests for all supported operating systems. No vendor has been selected.

The comparisons use the [Sentry Native SDK](https://docs.sentry.io/platforms/native/),
its [data handling documentation](https://docs.sentry.io/platforms/native/data-management/sensitive-data/)
and the [Crashpad design](https://chromium.googlesource.com/crashpad/crashpad/+/HEAD/doc/overview_design.md).
