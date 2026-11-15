# Bottom-of-topic read anchor

Issue #194 covers a topic whose loaded rows already say `read=true` while the
authoritative topic counter remains positive. The pinned reference's
`workspace-visible-message-read.hook.ts` schedules the server last-message UUID
with `allowReadAnchor` at the bottom; the normal read queue confirms and deduplicates
that boundary.

Reuse the native read queue, confirmation, counter refresh, owner/selection guards,
and confirmed-message set. Add a bottom-of-topic entry point that derives the
anchor from the current verified topic. Admit only a loaded matching last message,
positive topic unread count, a fully loaded tail, and no current read operation.
Keep ordinary visible-read admission unchanged. A confirmed boundary cannot be
submitted again just because an old counter remains positive.

The window dispatches this fallback only through its existing active visible-read
gate, after positioning/anchor handoff, at the actual bottom with no newer page.
Aggregate streams, inactive or obscured windows, older windows, unknown last
messages and stale routes must not trigger it.

The visible-window regression reproduced the mismatch on unchanged code: after
a real wheel scroll reached the tail, no read request was sent. It now verifies
one canonical request and refreshed zero counters in English/Russian and both
themes. Controller checks cover stale-counter and in-flight deduplication,
unknown/missing anchors, zero counters, aggregate routes and logout alongside
the existing confirmation and owner guards. Live-server and other-platform
acceptance remain separate from these synthetic native checks.
