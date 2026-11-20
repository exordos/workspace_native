# Report a problem

Open **Report a problem** from Diagnostics or the sign-in screen. Enter a short
description of what you expected, what happened and how to reproduce it. The
description remains in the form and can be copied separately for a support
conversation. **Prepare report** builds a local diagnostic preview. You can
include aggregate event counts and request timings or omit them. **Save report** exports the exact
JSON shown in the preview. Cancel or change accounts to discard the preview.

For a startup failure, run the installed native executable with
`--support-report` from a terminal. This writes a basic diagnostic report to
standard output before GUI, network, account or database initialization. You
can inspect that output and redirect it to a file of your choice.

## Format and resource limits

Schema 1 contains a random report ID, application version, the executable's
SHA-256 when available, package channel when identified, time and UTC offset, OS family, CPU architecture, Qt
version, display platform, language and theme. Missing data has an explicit
unavailable value. Optional counts use fixed severity names from the existing
session log. Optional timings summarize up to 200 requests in the current session
with median and 95th percentile latency. The exporter accepts up to 500 events and emits at most 64 KiB.
The binary fingerprint reads at most 128 MiB within a two-second processing
budget. Report preparation runs on a worker. Abandoning the form invalidates
the pending result and stops the fingerprint at the next read boundary.

The collector has no network transport. It never reads conversation text,
attachments, raw log files, server addresses, identifiers or environment
variables for the report. Keep personal details and credentials out of any
description you choose to share. A report ID links a reviewed file to a
support conversation. To reproduce a report, match its executable fingerprint
to the package, reproduce the described steps with a synthetic account and
run the owning feature's regression test.
