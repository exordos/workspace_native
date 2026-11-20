# Poll voters

An ordinary public poll shows the voters for a question when its results are
visible. Before voting, a poll configured to reveal results after voting keeps
other participants' identities hidden. Visibility is evaluated per question. A
closed or expired poll can show every question. Anonymous and read-only polls
do not request or display voter identities.

The option shows up to five overlapping 28 px avatars with 8 px overlap. Use
the identities supplied by the poll state first. For eligible options with
votes and no preview, request their first five voters, for at most five options
per poll revision and at most three simultaneous requests. The conversation
shares these requests across replacements of the same poll delegate. It retains
the lookup budget for that revision when older preview identities are evicted,
and cancels pending lookups when the poll becomes anonymous or read-only. A
failed preview leaves the count clickable so participants can open the list.

The option button opens its voter list. When the summary is visible on an open
non-quiz poll, View votes opens the overview. The overview groups its answer
selector by question. The dialog shows the selected option's participant count,
names, 32 px avatars and the applicable loading, empty or error message. It
offers Load more when the server returns a marker and Retry for a failed page.

Seed a page from the server-supplied preview. If further voters are known, the
last preview UUID becomes the first marker. Without a preview, fetch the first
page. Merge subsequent pages in order, once per UUID. A response belongs to the
account session, message, poll revision, question, option and request that
issued it. Closing the dialog or changing that selection retires its pending
result. Keep preview responses independent from foreground pagination.

The reference is the voter list and preview hooks, the poll adapter and the
voters dialog under `features/workspace-message-poll` in workspace_ui at
`230e02b8b0273dee556a406746d122180a68b3e7`. Native uses the existing poll
voters GET contract, account directory, avatar loader and theme colors.
Controller and window tests cover visibility, seeded and fallback lists,
pagination, retry, late responses and localized presentation.
