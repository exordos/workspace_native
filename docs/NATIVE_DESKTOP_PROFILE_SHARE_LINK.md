# Profile share link

The reference is `workspace_ui` `origin/master`, in
`workspace-profile-link.lib.ts` and `layout-profile-share-link.ui.tsx`.
The share hash is `#user/<uuid>`, case-insensitive and normalized to lower
case. Unlike the older issue description, the reference accepts all UUID
variants, opens a cached user immediately, and still requests a refresh.

The native external entry point is the existing `ew://open` route delivery.
An incoming profile hash is retained until the authenticated owner is ready,
then consumed once. Clear the drawer stack, resolve an owned user from the
in-memory or persisted directory, and request the current user projection.
Open the existing profile drawer on a matching result; reject stale-owner and
wrong-UUID, foreign-organization and stale-session replies. Successful
individual lookups are cached under the current owner with profile dates, so a
later offline launch can open them even if they were absent from the directory
snapshot. If the cache service closes before its read completes, continue
through the network path.
Validate the same user response fields as the web client. If resolution fails,
show the reference's dismissible inline notice; navigating away clears it.
A later navigation must not reopen a consumed link. Preserve the fragment when
a recognized external route has a query or an organization path.

Validation uses synthetic users and a real Quick window plus mock HTTP. Linux
checks do not establish live deployment, OS URL registration, Windows or macOS
acceptance.

For the failure notice, the current web source uses a 32 px small neutral
button and 12 px vertical padding with a 1 px bottom border. A local visible
browser reconstruction measured 57 px for a single line at both 1440 px
English and 700 px Russian viewport widths. This differs from the issue's
45 px estimate, which did not account for the button height. The notice grows
for wrapped text; the 14 px message uses a 20 px line height. The measurement
used the source classes and a saved Workspace CSS build, not a live service.
