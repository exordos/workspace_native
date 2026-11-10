# Shareable message links and desktop deep links

This document pins the native contract for message permalinks: the canonical
message URL, the `ew://` external protocol validation, the OS hand-off into a
running instance, and the anchor/highlight behavior when a link is opened.
It mirrors the Source client: the Electron protocol boundary
(`packages/electron/src/deeplink-route.lib.ts` at the pinned baseline), the web
route builder (`packages/web/src/shared/lib/workspace-messenger-route.lib.ts`
and `workspaceMessengerMessageRoute`) and the organization route segment
derived from the login origin (`packages/web/src/shared/lib/org-route.ts`,
`buildOrgRouteIdFromOrigin`). Deviation is limited to what the single-project
native session model requires.

## Canonical message URL

The canonical URL of a message is

```text
<origin>/org/<orgSegment>/project/<projectUuid>/message/<messageUuid>
```

- `origin` is the scheme, host and optional port of the Workspace server the
  current session authenticated against (the same origin the API client uses).
- `orgSegment` is exactly the Source organization route id derived from that
  origin: lowercase the hostname; append `-<port>` when the URL carries a
  non-default port; replace every remaining character outside `[a-z0-9.-]`
  with `-`; collapse runs of `-`; strip leading and trailing `-`; an empty
  result becomes `org`. The web client stores this same derivation at login
  time, so a link copied by the native client and a link copied by the web
  client for one message on one server are byte-identical.
- Every dynamic segment is percent-encoded with `encodeURIComponent`
  semantics (unreserved set `A-Za-z0-9-._~` stays literal). UUIDs are
  rendered lowercase, so encoding is identity for them.

`NativeController::messageLink(messageId)` returns this URL for any message
the controller currently owns, and an empty string when the message is unknown
or the session is not authenticated. `copyMessageLink(messageId)` puts it on
the clipboard; the message context menu item **Copy link** (`copyMessageLinkAction`)
uses the same visibility gating as **Copy text**.

The Source browser-versus-desktop "open in" preference (`app-message-entry.lib.ts`,
per-organization, 30-day TTL) is out of scope for this cut: the native client
is a desktop client and always handles its own links. That preference is a
tracked follow-up.

## External deep-link validation

The desktop protocol is `ew://open/<route>`. Validation is the Source
`parseExternalDeepLink` contract, reproduced with identical rejection
semantics:

1. The raw value is rejected when it contains a backslash or any character
   below `U+0021` (control characters and space).
2. The value must parse as a URL with scheme `ew:`, host `open` (comparison
   host-name case-insensitive), and no port, username or password.
3. The route is `pathname + search + hash`, with an empty pathname becoming
   `/`. The route must start with `/`, be at most 512 characters after
   trimming, must not start with `javascript:`, `data:` or `vbscript:`
   (case-insensitive) and must not contain `//`.
4. A route containing percent-encoded `%2F`, `%5C`, `%00`, `%0A` or `%0D`
   (case-insensitive) is rejected, so encoded-slash, backslash, NUL and
   newline escapes never reach the navigator.
5. A `/message/<segment>` pathname segment must be a UUID
   (`8-4-4-4-12` hex digits, case-insensitive).
6. Only the Source whitelisted shapes are accepted: `/`,
   `/org/<org>`, `/(calendar|mail|calls|licenses|settings|logs)` and their
   `/org/<org>/` forms, and complete project routes
   `/org/<org>/project/<project>/(messenger|inbox|feed|activity/<a>|message/<uuid>|stream/<s>(/topic/<t>)?)`
   with optional query and fragment.

`workspace::parseExternalDeepLink(value)` returns the accepted route or a
null optional; every rejection above has a unit test in the
`native-native-desktop` group.

## OS hand-off

- Cold start: the launcher passes the URL as a process argument. The last
  argument that validates as an external deep link wins (Source scan order).
- Warm start: `NativeInstance` extends its socket grammar by one payload.
  A second launch sends `activate\n` (unchanged, still the only payload when
  no URL is present) or `open <url>\n`, where `<url>` is the raw argument and
  the whole line is capped at 1024 bytes. The primary acknowledges every
  well-formed line with `ok\n`, oversized or unknown payloads still abort the
  connection as before.
- The primary never trusts the IPC channel: every received `<url>` is
  re-validated with the same parser before it is applied. An invalid URL is
  acknowledged but treated as a plain activation, so a spoofed socket can
  at most re-show the window. The channel still carries no credentials,
  commands or arbitrary payloads.
- The parsed route is applied when the session is ready. Before
  authentication it is held as a pending route, last route wins, and is
  flushed once the session is authenticated with a project selected (resume
  flow or interactive login).

## Applying a message link

Only `message` routes change navigation this cut; any other whitelisted route
only activates and shows the window (per-route application is tracked as
follow-up work). A message route applies as follows:

1. The `org` segment must equal the organization route id derived from the
   current server origin. A foreign organization lands in the safe state
   below without touching the selection.
2. The `project` segment must be the current project (then continue) or one
   of the signed-in account's projects (then switch via the existing project
   switch and continue there). An unknown project lands in the safe state.
3. The message is resolved authoritatively with
   `GET /api/workspace/v1/messenger/messages/?project_id=<project>&uuid=<uuid>`.
   A non-success response or an empty collection means the message does not
   exist or is not accessible to this account: the client sets
   `notice` to the unshared unavailable message ("The requested message is
   unavailable."), does not change stream, topic or selection, and stops.
   A single well-formed row names the target stream and topic.
4. The target opens through the existing conversation paths: a non-empty
   topic opens through `openTopic(topic, stream)`, a topic-less target selects
   the stream. The resolved message UUID becomes the pending anchor
   (`anchorMessageId`).

## Anchor, highlight and safe landing

- When the conversation history is ready, the message list scrolls the anchor
  message into view and highlights its row. The highlight clears 2.5 seconds
  after it is applied.
- If the anchor is not in the loaded window, the client requests bounded
  backward pagination through the existing older-page path: at most 8 pages
  of 50 (depth 400) while the anchor is pending. Finding the message stops
  the search and scrolls to it.
- When the resolution succeeded but the message is not found within the
  bounded depth, or pagination cannot reach it, the client keeps the opened
  conversation (a correct, safe location), clears the anchor and shows the
  same unavailable notice. No error path ever clears or changes the selection
  it was asked to leave alone (steps 1 to 3).
- The anchor is cleared by any user-initiated navigation (stream/topic
  selection, drafts page, project switch or sign-out), by logout, and by the
  highlight timer after a successful scroll. Cancellation applies only to
  navigation that moves away from the anchor destination. When the deep link
  itself starts resolving an uncatalogued target topic through
  `beginTopicOpening()`, that in-flight transition is owned by the anchor:
  its intermediate pre-commit state changes do not cancel the pending anchor.
  Only a different navigation, a superseded link or a failed topic
  resolution ends the search (issue #55).

## Locales

User-visible strings (`Copy link`, the unavailable notice) are en source
strings with reviewed Russian translations in `translations/workspace_ru.ts`;
the locale gate (lupdate completeness) must stay green.

## Test matrix

- `native-native-desktop` (headless): the full parser acceptance/rejection
  table from the contract above, org route id derivation (host, port, case,
  illegal characters, empty fallback), and the canonical URL builder shape and
  encoding.
- `native-native-controller` (synthetic server): link copy for a loaded
  message; deep-link open resolves via the lookup endpoint and opens the
  right stream/topic with `anchorMessageId` set, including a target whose
  topic and channel are absent from the loaded catalogs and are resolved
  through their detail GETs while the link resolves (issue #55); foreign
  organization, unknown project, empty lookup and failing lookup all keep
  the selection and set the notice; pending-route-before-login flush;
  `NativeInstance` socket grammar (`activate`, valid `open`, invalid `open`
  as plain activation, oversized payload abort).
- `native-native-window`: the **Copy link** menu item is visible and enabled
  for a readable message and puts the canonical URL on the clipboard; the
  anchor path scrolls and highlights a resolved message row.
