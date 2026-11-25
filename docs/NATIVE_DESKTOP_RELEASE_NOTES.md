# Workspace Native 0.49.0 - Linux development preview

- Image previews now open without waiting: encoded previews are kept in the on-disk cache and re-rendered from disk when you reopen a chat or restart the app, and the background prefetch now warms unread and recently opened chats beyond the selected stream (issue #729).
- Zoomed images in the media viewer can be dragged to pan (issue #730).
- Fixed: the chat wallpaper is bound to the actual message viewport and no longer drifts with the surrounding chrome (issue #733).
- Fixed: the AI summary dialog reloads its landing section when reopened (issue #738).
- Fixed: retired AI topic-summary work is settled at a session change, so a summary reply from the previous session can no longer surface in the new one (issue #736).
- Fixed: stale Zulip policy refresh replies are discarded after a confirmed save (issue #740).
- Fixed: the integration settings dialog now shows suspended state, timestamps and health counters (issue #752).
- Fixed: the restored window is framed with a palette edge outline and clamped back inside the screen work area, so a remembered window position can no longer push the composer below the screen edge (issues #720, #805).
- Fixed: startup paints the chats saved in the local cache before the refresh chain settles, instead of holding a blank first frame while a slow session refresh settles (issues #789, #802).
- Fixed: the notification-level segments in the topic context menu highlight on hover like the rest of the menu (issue #790).
- Fixed: keyboard focus moves to the composer when a reply starts, and the caret is handed back to the composer after a mouse click on Send (issues #779, #782).
- Fixed: the composer keeps your writing session when you switch topics in the sidebar, and stays reachable when the channel info drawer floats over the chat at a narrow window width (issues #780, #781).
- Fixed: the media viewer keeps its controls bar visible while a control in it has keyboard focus, and the play/pause and mute glyphs gained hover and pressed feedback (issues #784, #793).
- Fixed: Retry on a failed call embed re-creates the call view instead of stalling in the error state, and the Linux package now bundles the V8 startup snapshot the call renderer needs (issues #792, #804).
- Fixed: a failed saved-history refresh surfaces one alert with one Retry action instead of a duplicate banner pair (issue #786).
- Fixed: opening the About panel closes the account drawer layer first, so the App version row is reachable again, and auto read receipts now key off the account drawer's real state (issues #788, #791).
- Fixed: a declined image-preview reservation can no longer evict previews that were already cached, reservation eviction is atomic with admission, and derived previews are only admitted against derived cache entries (issues #775, #803).
- Fixed: warm topic preloads are fenced against history invalidation, so a late preload can no longer restore deleted messages, and retired preload slots are freed at a session renewal (issues #776, #777).
- Fixed: the integration settings entry shows pressed-state feedback (issue #794).
- Fixed: the provider limit fields in the integration settings dialog no longer hit a binding loop when a saved draft loads; the write-back follows your own edits only (issue #816).

The published Linux archive targets Ubuntu 26.04 x86_64 with glibc 2.43 or later. Existing Linux installations can use the built-in updater. Windows and macOS preview packages for x64 and ARM64 are available through the desktop packaging workflow.

[Full changelog from 0.48.1 to 0.49.0](https://github.com/exordos/workspace_native/compare/0.48.1...0.49.0)

---

# Workspace Native 0.48.1 - Linux development preview

- Fixed: the reconnect banner stays quiet while a realtime connection drop self-recovers; it now appears only when the connection actually needs your attention.
- The voice call window now loads: the QtWebEngine call runtime ships with the Linux package, and if the call shell still fails to load it settles cleanly so the window stays usable (issue #728).
- Fixed: topic-summary drafts are rebased per field and only the fields you edited are sent, so an in-progress draft no longer clobbers remote updates (issue #735).
- Fixed: a provider-policy conflict retry keeps the peer admin limits instead of dropping them (issue #739).
- Top-bar clusters and the chat header stay disjoint at narrow window widths (issue #726).
- Compact composer controls are centered with the one-line input row (issue #722).
- The member remove action now has its own column with a row-wide reveal (issue #723).
- The Add members modal is centered in the application area (issue #727).

The published Linux archive targets Ubuntu 26.04 x86_64 with glibc 2.43 or later. Existing Linux installations can use the built-in updater. Windows and macOS preview packages for x64 and ARM64 are available through the desktop packaging workflow.

[Full changelog from 0.48.0 to 0.48.1](https://github.com/exordos/workspace_native/compare/0.48.0...0.48.1)

---

# Workspace Native 0.48.0 - Linux development preview

- Clicking your name or avatar in the top bar now opens the Account page in the right drawer over the chat, mirroring the desktop client: profile and account controls open in place without leaving the conversation, and requesting Diagnostics from that page closes the account layer and opens diagnostics as before.
- Account disclosure rows now use the shared chevron icon family, so the account screens match the rest of the sidebar icon set.

The published Linux archive targets Ubuntu 26.04 x86_64 with glibc 2.43 or later. Existing Linux installations can use the built-in updater. Windows and macOS preview packages for x64 and ARM64 are available through the desktop packaging workflow.

[Full changelog from 0.47.1 to 0.48.0](https://github.com/exordos/workspace_native/compare/0.47.1...0.48.0)

---

# Workspace Native 0.47.1 - Linux development preview

- Fixed: unread badges on the folder rail now stay in sync when a chat is read - opening a conversation and reading it folds the unread counts of its folder and of the rail entry, the same read ledger the sidebar list uses.

The published Linux archive targets Ubuntu 26.04 x86_64 with glibc 2.43 or later. Existing Linux installations can use the built-in updater. Windows and macOS preview packages for x64 and ARM64 are available through the desktop packaging workflow.

[Full changelog from 0.47.0 to 0.47.1](https://github.com/exordos/workspace_native/compare/0.47.0...0.47.1)

---

# Workspace Native 0.47.0 - Linux development preview

- Videos in the media viewer now play like in the desktop client: a controls bar with play/pause, a draggable seek slider with elapsed and total time, mute and volume, and stop when you switch to another media item or close the viewer. If a video download fails, the viewer now offers a retry instead of an empty frame.

The published Linux archive targets Ubuntu 26.04 x86_64 with glibc 2.43 or later. Existing Linux installations can use the built-in updater. Windows and macOS preview packages for x64 and ARM64 are available through the desktop packaging workflow.

[Full changelog from 0.46.1 to 0.47.0](https://github.com/exordos/workspace_native/compare/0.46.1...0.47.0)

---

# Workspace Native 0.46.1 - Linux development preview

- Fixed: the sidebar no longer reorders chats when their older history is edited or swapped in - the row order follows its own stable keys instead of the currently loaded message window.
- Fixed: switching into a chat publishes its prepared topic history as one settled swap, so the topic list no longer flashes incomplete or stale rows mid-switch.

The published Linux archive targets Ubuntu 26.04 x86_64 with glibc 2.43 or later. Existing Linux installations can use the built-in updater. Windows and macOS preview packages for x64 and ARM64 are available through the desktop packaging workflow.

[Full changelog from 0.46.0 to 0.46.1](https://github.com/exordos/workspace_native/compare/0.46.0...0.46.1)

---

# Workspace Native 0.46.0 - Linux development preview

- Settings gained the topic-summary dialog: three tabs for the summary prompt and effort, per-workspace gate switches, and the endpoint registry. Administrators can add, edit and delete summary endpoints with server-side validation; users without permission see the controls as disabled instead of losing the screen.
- Fixed: a late endpoint reply can no longer overwrite a newer draft you started in the meantime - each save only touches its own submission, and the submitted key is consumed once accepted.

The published Linux archive targets Ubuntu 26.04 x86_64 with glibc 2.43 or later. Existing Linux installations can use the built-in updater. Windows and macOS preview packages for x64 and ARM64 are available through the desktop packaging workflow.

[Full changelog from 0.45.1 to 0.46.0](https://github.com/exordos/workspace_native/compare/0.45.1...0.46.0)

---

# Workspace Native 0.45.1 - Linux development preview

- Fixed: custom chat background photos are now rotated by the camera orientation metadata before they are saved, so a photo taken sideways no longer appears turned in the chat wallpaper.

The published Linux archive targets Ubuntu 26.04 x86_64 with glibc 2.43 or later. Existing Linux installations can use the built-in updater. Windows and macOS preview packages for x64 and ARM64 are available through the desktop packaging workflow.

[Full changelog from 0.45.0 to 0.45.1](https://github.com/exordos/workspace_native/compare/0.45.0...0.45.1)

---

# Workspace Native 0.45.0 - Linux development preview

- Server Diagnostics now shows a WebSocket activity summary: realtime state, the credential-free endpoint, per-event-type receive rates and the newest socket failure with its close code. Metadata only - no message payloads and no tokens are recorded.
- The client now checks for updates every hour, matching the desktop reference client.
- Fixed: the create-poll dialog opens centered in the window again instead of clipping past the top edge.
- Fixed: the media viewer now shuts down in bounded time even when a HEIC decode is stuck.
- Fixed: a chat background save that exceeds the storage cap no longer changes anything - both the live background and the stored configuration keep the last valid setup.
- Fixed: a late key-sharing sync merges only its own fields onto a row, so a newer rename or flag that landed while the request was in flight survives it.

The published Linux archive targets Ubuntu 26.04 x86_64 with glibc 2.43 or later. Existing Linux installations can use the built-in updater. Windows and macOS preview packages for x64 and ARM64 are available through the desktop packaging workflow.

[Full changelog from 0.44.0 to 0.45.0](https://github.com/exordos/workspace_native/compare/0.44.0...0.45.0)

---

# Workspace Native 0.44.0 - Linux development preview

- The client confirms an installed update once on the next start, so you can see the new version landed instead of guessing.
- Settings gained an integrations entry: administrators can manage external providers - probe them, resolve policy conflicts and use the emergency actions.
- Composer attachments for HEIC photos now show a decoded preview while you compose, and the preview decode is cancelled the moment the attachment is removed.
- Fixed: your global account screens stay reachable when you navigate back from Calendar or Mail, the sidebar keeps your scroll position when you switch chats (and yields it instantly when you scroll yourself), and a topic rename that replies late no longer corrupts a retired stream's channel list.
- Fixed: attachment previews are now classified from the confirmed upload metadata, and the pending-update marker survives updater runs and install-root changes.

The published Linux archive targets Ubuntu 26.04 x86_64 with glibc 2.43 or later. Existing Linux installations can use the built-in updater. Windows and macOS preview packages for x64 and ARM64 are available through the desktop packaging workflow.

[Full changelog from 0.43.0 to 0.44.0](https://github.com/exordos/workspace_native/compare/0.43.0...0.44.0)

---

# Workspace Native 0.43.0 - Linux development preview

- HEIC/HEIF photo support: attachment tiles, the media viewer and the composer preview now decode Apple's high-efficiency format.
- Chat backgrounds: pick a local preset or set your own image, per chat.
- The sidebar chat search opens from the keyboard (Ctrl+K), moves with the arrow keys and closes on Escape.
- Topic context menu: rename, subscribe and manage a topic straight from the channel list.
- Clicking a message author's avatar opens their profile in the right sidebar.
- The chat rail, stream sidebar and right drawer unmount when you switch to Calendar or Mail, so non-chat screens keep their full width.
- Encrypted channels: the owner can now turn key sharing on or off from the channel settings.
- The poll trigger moved into the expanded composer toolbar, and attachment cards react across the whole card and activate with Enter or Space.
- Fixed: attachment download cards for links inside formatted message text, attachment cards built from rendered Markdown links, stale topic actions after a sign-in renewal, and a media-viewer crash guard for late HEIC decodes.

The published Linux archive targets Ubuntu 26.04 x86_64 with glibc 2.43 or later. Existing Linux installations can use the built-in updater. Windows and macOS preview packages for x64 and ARM64 are available through the desktop packaging workflow.

[Full changelog from 0.42.0 to 0.43.0](https://github.com/exordos/workspace_native/compare/0.42.0...0.43.0)

---

# Workspace Native 0.42.0 - Linux development preview

- Search cached messages in the current account and project, or use the workspace directory to find a person and open a direct conversation.
- Preview a message before sending it, resize the composer with the mouse or keyboard, and reply to several selected messages at once.
- Inspect poll voters when the poll allows it, manage an open poll you created, and include a plain-text summary when quoting or forwarding a poll.
- Older messages load ahead of backward scrolling. Incoming messages can arrive while a page loads, and the client keeps your place when it inserts the older rows.
- Customize window shortcuts in Settings. From Diagnostics or the sign-in screen, you can also preview a local support report and choose what to save.
- Attachment cards in chat show live download state: a progress ring with percent while a file saves, Open once it is on disk, and Retry when a download fails.
- Opening the message timestamp viewer moves keyboard focus into the dialog and returns it to the message on Escape, so screen readers follow it (Linux).

The published Linux archive targets Ubuntu 26.04 x86_64 with glibc 2.43 or later. Existing Linux installations can use the built-in updater. Windows and macOS preview packages for x64 and ARM64 are available through the desktop packaging workflow.

[Full changelog from 0.41.0 to 0.42.0](https://github.com/exordos/workspace_native/compare/0.41.0...0.42.0)

---

# Workspace Native 0.41.0 - Linux development preview

Message timestamps become inspectable, mirroring the reference time viewer:

- Clicking (or Enter/Space on) a message time chip opens a read-only
  timestamp panel anchored to the chip: a 371 px card showing the exact
  stored instant - the localized month caption with the stored day
  highlighted under its weekday column, and the time rendered to the second
  whenever the instant carries non-zero seconds. Nothing in it is editable,
  Escape closes it and returns focus to the chip, the content scrolls when
  the window is short, and screen readers announce the full instant. The
  `urn:time:` reference kind now opens the same panel instead of the
  "under development" notice.

---

# Workspace Native 0.40.0 - Linux development preview

Calls put screen sharing behind a real permission gate with a native source
picker, and protected chats carry their reference header identity:

- The call window now routes every display-capture request through a named,
  test-pinned permission gate and implements the native screen-source picker
  contract over the XDG ScreenCast portal (session, source selection, per
  request isolation). Because this Qt runtime exposes no trustworthy frame
  identity for capture requests, the gate deliberately fails closed: screen
  sharing stays denied - exactly as in the previous release - until the
  runtime provides the missing signal, and no fabricated grant path exists.
- The encrypted conversation header matches the reference: the lock avatar
  paints the selected stream color as its background (invalid or absent
  colors fall back to the theme surface), and the protected-chat tooltip
  grew into the reference surface - title plus the five protection bullets
  in both locales, reachable by keyboard.

---

# Workspace Native 0.39.0 - Linux development preview

The sidebar rows grow their reference state badges, the chat header reports
membership, an active call keeps the display awake and announces itself in
the tray, and the folder rail recovers from a renewal-interrupted write:

- Sidebar stream and direct rows now mirror the reference row states: direct
  rows carry the partner status line, pinned rows show the pin marker, rows
  with an unread mention carry the mention badge, and passive-only unread
  renders the counter in the muted gray variant.
- Muted conversations are visible at a glance: muted stream rows and topics
  with a mute override carry the crossed-bell glyph, following topics carry
  the follow glyph, and effectively silent rows render dimmed. The stream
  context menu offers the reference notification levels - all messages,
  mentions only, muted - switches them in place, and locks each switch while
  its write is in flight.
- The chat header shows the member and online counters for channels and
  topics under the title - singular forms handled - and the encrypted-channel
  header now gets its own member caption instead of an empty placeholder;
  direct chats keep the partner sub-line instead.
- While a call is active the client asks the desktop to stay awake, releasing
  the request when the call ends even if the desktop service answers late,
  and the tray tooltip names the ongoing call in both locales.
- Folder operations interrupted by a session renewal no longer strand the
  folder rail: the interrupted mutation is reconciled after re-authorization
  - including a write that commits later than the check - so the rail
  converges on the server state without a restart.

---

# Workspace Native 0.38.0 - Linux development preview

The diagnostics page grows the live memory panel, the folder rail gains its
reference layout options, and direct chats gain the partner status caption:

- The memory section polls every three seconds and refreshes in place: the
  JS heap card carries its heap-limit detail and a usage progress bar, the
  total app memory, system free memory and device RAM cards report the
  machine facts, and the process table lists Type, PID, Working set, Peak
  and CPU rows. A free-memory reading the OS does not report is left out
  instead of being shown as zero, and the table scrolls horizontally rather
  than clipping its columns in narrow windows.
- The folder rail now offers both reference layouts: the vertical list and
  the horizontal chip strip, toggled from the rail's context menu together
  with the show-system-folders option, and both choices persist across
  restarts. The horizontal rail scrolls under pointer drag, every chip is
  reachable with Tab and activatable with Return or Space, and the selected
  chip keeps the reference visuals.
- The direct chat header resolves its sub-line in the reference precedence:
  deactivated, then typing, then the custom status with its emoji, then
  presence and last seen. The whole partner block is now a real focusable
  button that opens the profile drawer from the keyboard with a visible
  accent focus ring, in both locales and both modes.
- The diagnostics page now scrolls with mouse wheel and touchpad gestures, so
  the advanced snapshot section stays reachable on short windows: the page body
  follows every scroll gesture and the advanced row can be scrolled into view
  and opened without leaving the viewport.

---

# Workspace Native 0.37.0 - Linux development preview

The folder rail becomes fully editable and the diagnostics page gains the
reference session log viewer:

- Folders are created, renamed and deleted from the rail: the create and
  rename modals carry the name field and the color picker with a live
  preview, the delete asks through the reference confirmation dialog,
  system folders stay non-editable with rename and delete disabled in the
  context menu, failures land as toasts, and the rail updates incrementally
  without a full refetch - a stored black folder color stays distinct from
  an unset one in the rename prefill.
- The session log viewer opens over the diagnostics page and appends entries
  live: the search box with the level, scope and runtime filters narrows the
  list under a reset control and the "X of Y" results label, a recent-errors
  block stays open while the visible window has errors, Clear empties to the
  reference empty state, Download exports a pretty JSON array with entry
  payloads passed through the logger redaction, warn and error rows carry
  their reference side tints, transport API failures reach the log with
  their failure reason, and a failing export reports its error inside the
  modal instead of overflowing it.

---

# Workspace Native 0.36.0 - Linux development preview

The profile menu gains the runtime diagnostics page, F11 toggles fullscreen
like the reference client, and the Favorites rail chip opens the personal
self-chat:

- Diagnostics opens from the profile menu and renders the reference
  connection dashboard: the status pill paints healthy green, degraded yellow
  or offline red from live collected state, nine cards show connection,
  realtime, cache and memory facts with their measured values, Ping server
  runs a real round-trip with millisecond feedback and an automatic reset,
  Copy snapshot lands the JSON report with its two second feedback, and the
  advanced snapshot block toggles open - all in both locales and modes.
- F11 toggles window fullscreen exactly like the reference View menu
  accelerator (which lives under the hidden menu bar): only the normal bounds
  and the maximized flag are persisted, a user window move or restore made
  while the window manager settles the transition is recaptured instead of
  being saved as fullscreen geometry, and detaching during the transition
  keeps the settled state, never the transient one.
- The sidebar Favorites chip is enabled and opens the "Personal notes"
  self-chat with the reference favorites header: the private self stream is
  found or created lazily per session with in-flight deduplication and retry,
  the composer behaves as in any direct chat, and the self stream stays out
  of the ordinary sidebar and folder lists.

---

# Workspace Native 0.35.0 - Linux development preview

The sign-in wizard picks its workspace like the reference, the chat list
honors the reference layout settings, and a message link to an unloaded
message shows the reference transition instead of a blank wait:

- The login project step is a real dropdown: the organization suffix joins
  the project name, the em-dash placeholder matches the reference form, and
  a long project list scrolls inside the popup. A profile without any visible
  project gets the blocking notice before the wizard proceeds, and a deep
  link that names a specific project is enforced against the picked one.
- The chat list follows the two reference layout settings persisted with the
  session: compact density tightens the row ladder to the measured
  reference geometry and drops the preview line, timestamp and avatar, while
  the open-latest-topic click mode sends the row body to the last message
  topic and keeps the avatar as its own stream link named `General Chat: <title>` for keyboards and screen readers.
- A message link for a message outside the loaded window shows the reference
  skeleton preview zones while the anchor resolves - static under the
  platform reduced-motion setting - and a failed resolution lands an in-list
  alert whose Retry re-issues the fetch.

---

# Workspace Native 0.34.0 - Linux development preview

The sidebar chat list now accounts for every catalog state and the Activity
rail opens into the reference expanded panel:

- A first load without rows shows the dashed spinner card with "Loading...",
  a failed catalog load replaces the list with the reference blocking error
  card, and a partial refresh keeps the rows on screen with the
  "Some sidebar data could not be refreshed" banner above the list. The five
  reference empty states render in both locales: "No matches" with its hint
  for a search miss, "No workspace chats yet", "No direct messages yet",
  "No channels yet" and "Folder is empty". Every state announces its text to
  assistive technologies.
- The Activity chevron expands the compact rail into "My Activity": five rows
  with the reference-colored chips (purple, blue, red, yellow, pink) and
  unread count badges on inbox and mentions matching the rail badges, with
  the open state persisted per workspace session. Favorites stays disabled
  until the favorites page ships.

---

# Workspace Native 0.33.0 - Linux development preview

The Open Source Licenses browser opens from the About drawer and its deep
link, matching the reference page in both locales:

- The Licenses page renders the native client's own third-party dependencies:
  the top eight license-type chips with package counts (toggle-off and a Reset
  pill included), case-insensitive search over package name and publisher, the
  "{filtered} of {total} packages" counter, and a sticky-header table whose
  package cells link out only when a repository exists. Closing resolves like
  the reference: back to the section that preceded the open, otherwise the
  messenger home.

---

# Workspace Native 0.32.0 - Linux development preview

The Services section opens its placeholder hub, the Activity reactions route
answers honestly that reactions are not connected yet, and a restart with a
saved realtime cursor reopens the chats instead of leaving them empty:

- The Services section opens the reference placeholder hub: service cards in
  the reference grid (3 columns on wide windows, 2 then 1 as the window
  narrows) each carry their planned-status chip, the header hint wraps instead
  of overflowing at minimum width, and the "Chats & channels" button is fully
  keyboard-operable with a visible focus ring and returns to the messenger.
  Services open through their deep link and from the sidebar entry.
- The Activity reactions collection route shows the reference unsupported
  state in both locales ("Reactions are not connected to Workspace messaging
  yet.") instead of falling through to the default activity view, and leaving
  for a conversation closes that view cleanly. Message reactions themselves are
  unchanged.
- After a restart that restores the durable realtime cursor, the fresh
  controller always bootstraps from an acknowledged authorized snapshot: chats
  no longer stay empty until a manual refresh, and a reconnect while the
  restore snapshot is still in flight keeps waiting for its acknowledgement
  before any catch-up starts.

---

# Workspace Native 0.31.0 - Linux development preview

Files in the composer become tracked cards, a shared profile link opens the person it names, people show their real photos and presence, and the Calls and Marked-messages pages arrive:

- Composer attachments now get a card per file: live progress, cancel, retry
  and removal, files added by drag-and-drop or pasted straight from the
  clipboard, and a send that waits for every upload while the text stays
  editable. A reopened edit with a leading attachment keeps its reply tab.
- A shared profile link (#user/<uuid>) opens the native profile drawer for
  exactly that person: the profile is taken from the local cache when known,
  the navigation happens once per link, and an unknown target gets the
  reference failure notice instead of a silent no-op.
- Avatars carry real photos: a Gravatar reference resolves through the
  bounded avatar pipeline pinned to the Gravatar origin, the fetched photo
  is masked into the circles that already exist, initials stay as the
  fallback and a failed fetch retries on the next mount.
- Channel and group rows in the sidebar paint the avatar background in the
  stream's own color, matching the reference sidebar.
- A direct chat with a deactivated account shows the deactivated badge on the
  partner avatar in the header instead of the presence dot, with the
  reference badge geometry and accessibility name in both locales.
- The Calls section opens its reference page: the recent Jitsi calls list
  with an honest empty state when no call has happened yet.
- Marked messages join the Activity surfaces: a starred page, paged with
  markers and auto-loading its first page, opens the conversation any row
  came from.

---

# Workspace Native 0.30.0 - Linux development preview

Sign-in gains its own one-time-code step, and the sidebar learns to browse several chats at once:

- When the identity provider asks for a one-time code, the sign-in card now shows a dedicated code step instead of one crowded form: a rejected code stays on that step and can be retried, the retry stays bound to the original login attempt and server, and Back returns to the credentials with the fields preserved, exactly as in the reference client.
- Chats in the sidebar now expand in place: open a chat's topics without leaving your current conversation, keep expanded chats per workspace session, and reveal unfinished and completed topics in stages behind show-more levels, matching the reference sidebar behaviour.

---

# Workspace Native 0.29.0 - Linux development preview

Reading position survives restarts, forwarding becomes an honest batch, and the project feed gets its own page:

- Realtime stream positions are now durable: each conversation's
  cursor is stored locally per owner and restored when the socket
  resumes - a resynchronize that lands after the acknowledgement replaces
  the restored position, a failed persist is retained and retried until a
  read-back confirms it instead of being silently dropped, everything is
  wiped on sign-out, and a corrupt store degrades to a clean fallback
  rather than blocking delivery.
- Forwarding several messages now lands exactly one copy per message:
  select rows from the bubble menu or row checkboxes and forward through
  the reference modal with stream and topic pickers. Duplicates inside a
  batch collapse into a single delivery, a posted batch is confirmed by a
  GET read-back and never re-sent, and encrypted or unknown destinations
  are refused before anything leaves the machine. Messages the client
  cannot render reject the batch instead of being silently omitted, and a
  confirmation can no longer overwrite a newer selection.
- The project feed page lists every message across all of a project's
  conversations in one place: deep links open it, top pagination keeps
  fetching older pages, and open-in-chat jumps to any row. Preview text
  renders markdown safely - link labels survive their destinations,
  which stay hidden.

---

# Workspace Native 0.28.0 - Linux development preview

The taskbar now reports update downloads, and Calendar joins the top bar:

- While the updater is downloading an update, the launcher tile in the
  taskbar mirrors the download progress and shows the completed hint when
  the download finishes. A listener that joins the session bus mid-download
  is no longer left behind: the progress hint is republished on an interval
  while a download is in flight and stops once the update reaches a terminal
  state.
- The Calendar button in the top bar is now active instead of a disabled
  placeholder. Clicking it switches to the calendar section as its own page,
  rendered as the reference unavailable-state card: centered glyph, heading
  and body text explaining that calendar is not available in the packaged
  desktop client, identical in both locales and both color modes. The chip
  follows the reference idle, active and hover behavior.

---

# Workspace Native 0.27.0 - Linux development preview

Mail joins the top bar as a real section, and typing in busy conversations got cheaper:

- The Mail button in the top bar is now active instead of a disabled
  placeholder. Clicking it switches to the mail section as its own page,
  rendered as the reference unavailable-state card: centered glyph, heading
  and body text explaining that mail is not available in the packaged
  desktop client, identical in both locales and both color modes. The chip
  follows the reference idle, active and hover behavior down to the subtle
  hover border on idle chips.
- Typing in the composer no longer rebuilds the message list projections,
  so open conversations stay responsive while you write.

---

# Workspace Native 0.26.0 - Linux development preview

Four reference surfaces joined the client, plus a batch of fixes from the field:

- Conversation deep links are real: a stream or topic link now opens that
  conversation natively - canonical UUIDs and encoded names, default-topic
  tokens, and the honest unavailable notice for foreign or missing
  destinations. Links queued before login dispatch once the session is
  verified, and a newer link replaces an older queued one.
- Failed attachment downloads no longer vanish: a download center shows byte
  progress, lets you cancel, open or reveal a finished file, and retries a
  failed row in place - retry waits for a verified session refresh and reuses
  the same file identity and name. Removing a download record never deletes
  the saved file.
- Opening a conversation lands on the first unread message and restores the
  previous reading position, matching the reference navigation.
- Editing a message now manages its existing attachments: removable
  attachment cards survive the edit session.
- Image attachments whose bytes do not match their extension are rejected at
  selection with the localized attachment error; the validated bytes are the
  bytes that go on the wire, even if the source file changes mid-flight.
- Sidebar search now matches topic names and direct-chat participants, not
  only stream titles.
- Unread counters reconcile from the last visible topic message, so a counter
  never shows messages you have already scrolled past.
- A conversation link queued before login survives startup that opens Inbox:
  the dispatcher waits for the verified catalog and hands the link to the
  authorized route handler.
- Composer formatting actions are keyboard accessible.
- Meeting links that embed IPv6 literals inside message text are recognized.
- Quote thumbnails renew their file leases when the image scope changes, so
  previews survive credential renewal.
- A saved conversation that the server no longer offers is quietly retired
  instead of erroring on every start.
- Token renewal no longer strands key operations: an in-flight key reissue or
  rotation check settles as interrupted instead of holding the busy marker
  for the rest of the session (durable rotation pins are untouched), and a
  late definitive recovery answer can no longer update evidence for a
  creation attempt the user already discarded - the fence and its regression
  tests are deterministic regardless of stage scan order.

---

# Workspace Native 0.25.1 - Linux development preview

Bug-fix release on top of 0.25.0:

- A poll whose creation response arrives after you navigated away now lands
  only in its original topic and channel aggregate - it no longer inserts
  itself into whichever conversation is open when the answer arrives.
- The media viewer now owns the keyboard while it is open: typing and Enter
  stay inside the viewer, the covered draft or edit can no longer be changed
  or submitted, and the previous focus and text come back when it closes.
  Read receipts pause while media covers the conversation and resume after.
- Attachments added while editing a message belong to that edit session:
  they appear in the edited body only, never in the conversation draft, and
  cancelling or replacing the edit cancels only its own uploads.
- Startup no longer eagerly rebuilds the user directory for hidden direct
  chat, channel-creation and add-members pickers; each picker reads the
  current directory when it actually opens. This removes one measured
  startup cost on the way to the broader responsiveness work.
- Encrypted quote previews became resilient: retry now works for a quoted message
  fetched outside the loaded history, a confirmed edit of the source message
  invalidates the stale encrypted preview so old plaintext is never shown
  again, and pending quote lookups retire with their session instead of
  retrying under another account.
- User profile drawer: the Message action opens the right direct chat even
  when the directory has no default-topic pointer, an older directory
  refresh can no longer overwrite a freshly uploaded or removed avatar, and
  a saved About-me description stays visible after load and save.
- Quick reactions no longer hide behind neighbouring bubbles: the reaction
  capsule is raised above the neighbours so overlapping emoji stay visible
  and clickable, and the mouse wheel belongs to the capsule while its
  overflowing list scrolls - a list that already fits never moves the
  conversation behind it.
- Access-token renewal no longer strands encrypted-channel creation work:
  an interrupted status check settles honestly as unresolved (the staged
  channel key survives), a confirmed Discard is no longer blocked by the
  stale in-flight marker, and automatic recovery re-arms right after the
  renewal or a re-login - no restart needed.
- Fixed a crash where a late definitive answer from a recovery check could
  reach a creation attempt the user had already discarded.

---

# Workspace Native 0.25.0 - Linux development preview

The protected-chat cluster and two chat surfaces landed from the reference desktop:

- Encrypted channels can be created right in the create-chat dialog: the
  protected switch generates the group key before the channel request is sent,
  keeps it durably staged until the server confirms the channel, and recovers
  an interrupted creation after a crash or restart - the channel key survives
  every failure path. A stuck creation is never silently lost: an inventory of
  unresolved creations offers Check and an explicitly confirmed Discard.
- Encrypted chats now have the protected chat info drawer with the reference
  member management (48 px rows with presence dots, owner-gated add and
  remove) and the key management screens: channel key details, local key
  inventory, key rotation with a fail-closed dispatch (nothing leaves the
  device unless the durable rotation marker was written first) and deletion
  with a confirm step. Stalled rotations surface a dormant badge and join the
  same user-resolvable inventory as create stages.
- The folder rail is real: folders load from the server with unread badges,
  folder colors and the pinned All row first; selecting a folder scopes the
  sidebar to that folder's chats.
- Copy image landed: the message context menu and the media viewer copy a
  message image onto the system clipboard silently, with the same 64 MiB
  reference limit, and a copy that finishes after its session was retired is
  discarded instead of overwriting the clipboard.
- The client now understands power events: after a suspend/resume cycle all
  realtime connections reconnect immediately, and the presence heartbeat
  widens while the window is hidden or the machine runs on battery, matching
  the reference cadence ladder.
- Diagnostics gained a persistent rotating log file and a memory snapshot
  provider for support flows.

Existing installations download available updates automatically and offer
**Install and restart**. For a new per-user installation, extract the archive
and run `Exordos Workspace`; see README for the manual install path.

---

# Workspace Native 0.24.0 - Linux development preview

Two surfaces ported from the reference desktop:

- Poll bubbles are now fully interactive. Click any option to vote and the
  bubble updates in place with the vote count, per-option percentage and the
  participant line, anonymous polls keep voters hidden from the aggregate, and
  the closing deadline shows on the card. The poll owner can add a new option
  inline; an option under creation keeps its identity across retries, so an
  unstable network cannot duplicate it. Narrow windows are covered down to the
  smallest supported bubble: when a result row runs out of room, the aggregate
  drops onto its own line instead of overflowing the bubble.
- Replying can now quote exactly what you selected. Drag to highlight text
  inside a message bubble and pick Reply: the composer preface and the sent
  quote carry the selected fragment (line breaks preserved); a whitespace-only
  or encrypted selection falls back to quoting the whole message, matching the
  reference behavior.

Existing installations download available updates automatically and offer
**Install and restart**. For a new per-user installation, extract the archive
and run `bash install.sh`.

[Full changelog: 0.23.0...0.24.0](https://github.com/exordos/workspace_native/compare/0.23.0...0.24.0)

---

# Workspace Native 0.23.0 - Linux development preview

Two surfaces ported from the reference desktop:

- Reply and quoted-message previews now lead with a media thumbnail: when the
  quoted message contains an image or a video, the composer reply preface and
  the in-bubble quote frame show a rounded 48 px preview of the first image
  (first video when the message carries only video), matching the reference
  geometry. A media preview that fails to resolve falls back to the previous
  text-only quote layout, and quotes that contain only media keep their media
  description audible for assistive technology.
- Nested quote frames are settled to their final geometry before the message
  list restores its scroll position, so viewport restoration no longer races
  quote measurement when opening topics with deep quote chains.

Existing installations download available updates automatically and offer
**Install and restart**. For a new per-user installation, extract the archive
and run `bash install.sh`.

[Full changelog: 0.22.0...0.23.0](https://github.com/exordos/workspace_native/compare/0.22.0...0.23.0)

---

# Workspace Native 0.22.0 - Linux development preview

Two surfaces ported from the reference desktop:

- Polls are now created without leaving the composer: the composer's poll icon
  opens the creation dialog, where you add the question and answer options, set
  an optional closing deadline and create the poll. Creation is retry-safe - an
  unstable network cannot duplicate a poll, and a retried send reuses the same
  identity - while a send is in flight the dialog keeps its shape: closing and
  cancelling stay locked until the server answers, an unrelated message arriving
  cannot dismiss it, oversized question or option lists are rejected by the form
  instead of silently pending, and an expired deadline is refused rather than
  quietly dropped. Poll messages render as the poll placeholder for now; the
  interactive poll bubble arrives in a later release.
- The mouse wheel scrolls the way the reference desktop does: one notch moves
  the message list or the channel sidebar by the measured reference distance of
  100 px, repeat notches no longer accelerate, and touchpad scrolls keep their
  fine-grained native path. When a list is already at its end, an outward wheel
  is handed back instead of swallowed, so surrounding content can still receive
  it.

Existing installations download available updates automatically and offer
**Install and restart**. For a new per-user installation, extract the archive
and run `bash install.sh`.

[Full changelog: 0.21.1...0.22.0](https://github.com/exordos/workspace_native/compare/0.21.1...0.22.0)

---

# Workspace Native 0.21.1 - Linux development preview

One fix for existing behavior:

- Restoring the window after maximizing no longer loses your place. The
  conversation tail stays visible when you were at the newest message, and a
  reader parked in older history keeps its anchor row on screen instead of
  being dumped into empty space or dragged to the tail.

Existing installations download available updates automatically and offer
**Install and restart**. For a new per-user installation, extract the archive
and run `bash install.sh`.

[Full changelog: 0.21.0...0.21.1](https://github.com/exordos/workspace_native/compare/0.21.0...0.21.1)

---

# Workspace Native 0.21.0 - Linux development preview

- A call now starts from the people and conversations around you, not only from
  a call card: a Call action joins the peer's profile card and the chat info
  panel, and it creates the direct conversation on the spot when none exists
  yet. A refusal cannot leave a call armed for later - declining an unusable
  creation queues nothing, a call attempt started before a re-login can never
  report into the new session, and for an encrypted direct message the action
  stays disabled until the conversation's key is actually ready.
- Calls now survive the real Qt WebEngine they run in. The meeting window reacts
  to the browser's true load status, so a successful connect leaves *Loading*,
  a failed one shows its reason and can be retried, and mic and camera requests
  from the meeting are actually granted - only to the meeting's own origin,
  while events arriving from any other frame or origin are dropped instead of
  forging a roster change or a self-feeding readiness loop. Participant count
  and roster now arrive through the external API's own wire events.
- Polls got their foundation: poll payloads are parsed and validated against the
  reference contract, survive cache round-trips, and the REST actions for
  voting, closing a poll, changing settings, adding an option and listing
  voters are wired with realtime merge. Poll bubbles themselves come in a
  later release.

Existing installations download available updates automatically and offer
**Install and restart**. For a new per-user installation, extract the archive
and run `bash install.sh`.

[Full changelog: 0.20.0...0.21.0](https://github.com/exordos/workspace_native/compare/0.20.0...0.21.0)

---

# Workspace Native 0.20.0 - Linux development preview

Three features ported from the reference desktop:

- Video now plays inside the media viewer instead of only offering a download:
  the viewer pages between image and video slots with a position counter, a
  video slot shows its own loading text, then plays with inline controls, and
  load and display failures get their own distinct messages in both locales.
  Video entries in the thumbnail strip carry a play badge, and oversized video
  lists are trimmed to what the viewer can actually render.
- An incoming call stops being silent: a large invite card for a normal window
  and a compact one when the window is narrow now appear with the caller's
  name and photo, answer and decline actions, a video toggle and automatic
  decline after 45 seconds if nobody answers. The invite keeps its video
  preference tied to the call that raised it.
- Encrypted messages that could not be decrypted no longer stay dead. The error
  row gained a per-message retry that asks for the key again and decrypts in
  place, so a message you were given the key for later can be recovered without
  leaving the conversation.

Existing installations download available updates automatically and offer
**Install and restart**. For a new per-user installation, extract the archive
and run `bash install.sh`.

[Full changelog: 0.19.0...0.20.0](https://github.com/exordos/workspace_native/compare/0.19.0...0.20.0)

---

# Workspace Native 0.19.0 - Linux development preview

Three features ported from the reference desktop:

- Joined calls now get their own window: an active meeting opens in an expanded
  call window with header controls (mute, camera, hand, hang up) and collapses
  to a picture-in-picture tile that stays on top and remembers where you put
  it. The web meeting keeps a single embed across expand and collapse, the
  window follows the meeting into fullscreen and back, and the participant
  count tracks the live meeting through the embed's own event channel instead
  of a guess. Camera, microphone and screen access are only granted to the
  meeting's own origin, never to a page it redirects to.
- A reply you did not finish now survives navigation: leaving a conversation
  parks the whole reply session - every quote tab, its answer and the active
  tab - under that conversation, and returning restores it intact. Cancelling
  the reply drops the parked copy, and a quote whose text just changed or went
  empty shows the current excerpt rather than a stale memo.
- Editing a message that begins with a quoted line restores the reply session
  for the edit: the quote chip shows above the composer, the composer is
  pre-filled with only the answer, and saving re-serializes the quote line
  byte-for-byte. Quote lines inside code blocks are never mistaken for reply
  quotes, and a restored chip keeps its parsed sender label while the quoted
  origin is still loading or turns out to be unavailable.

Also fixed: a reply quoting a message outside the loaded window now shows the
resolved excerpt of that quote once the lookup lands.

Existing installations download available updates automatically and offer
**Install and restart**. For a new per-user installation, extract the archive
and run `bash install.sh`.

[Full changelog: 0.18.0...0.19.0](https://github.com/exordos/workspace_native/compare/0.18.0...0.19.0)

---

# Workspace Native 0.18.0 - Linux development preview

One feature ported from the reference desktop:

- The composer can now answer several messages at once. While a reply is open,
  choosing Reply on another message stacks a second quote tab instead of
  replacing the first: each tab holds its own answer, tabs switch and close
  individually, and sending posts one message that answers every tab in tab
  order. Mentions typed into any tab stay real mentions, and each saved answer
  is checked against the same size and protected-content rules as an ordinary
  draft. A quote re-pointed while a send is in flight no longer closes the
  reply session.

Existing installations download available updates automatically and offer
**Install and restart**. For a new per-user installation, extract the archive
and run `bash install.sh`.

[Full changelog: 0.17.0...0.18.0](https://github.com/exordos/workspace_native/compare/0.17.0...0.18.0)

---

# Workspace Native 0.17.0 - Linux development preview

Two features ported from the reference desktop:

- Chats can start and join Jitsi meetings: when the workspace configures a meet
  host, the chat header shows the call action, and a meeting link posted in a
  chat renders as a call card with the room name, participant initials and a
  join button. The room name follows the reference six-segment contract, a
  meeting only opens after the server confirms the message carrying its link,
  and only one call can be active at a time - starting a different one is
  refused with an inline notice before anything is sent. The embedded call
  window ships in the next release.
- Idle sessions now sign themselves out: after the configured idle window
  (6 hours to 7 days, default 3 days, with "never" to disable), the app signs
  out through the same path as a manual sign-out, so stored credentials are
  wiped the same way. The timer follows your keyboard and mouse activity, pauses
  while every window is hidden or minimized, and the setting is remembered per
  account. There is no countdown warning, matching the reference.

Existing installations download available updates automatically and offer
**Install and restart**. For a new per-user installation, extract the archive
and run `bash install.sh`.

[Full changelog: 0.16.0...0.17.0](https://github.com/exordos/workspace_native/compare/0.16.0...0.17.0)

---

# Workspace Native 0.16.0 - Linux development preview

Two features ported from the reference desktop:

- The right drawer now opens the full user profile: avatar, name, username and
  role chips with the About-me text; an edit mode for the display name, About me
  and email that saves only changed fields; an avatar change dialog with upload
  preview and reset (image content is validated, not just the file name); and
  direct actions to message the person or copy their share link. Editing is
  unavailable until the profile has loaded so a slow response can never wipe
  existing values.
- Chat history now prefetches older messages before you reach the top: one
  viewport of pages is requested while scrolling, delivered pages insert at
  their anchor without moving the visible message, and repeated gestures never
  queue duplicate requests - reaching the top continues seamlessly.

Existing installations download available updates automatically and offer
**Install and restart**. For a new per-user installation, extract the archive
and run `bash install.sh`.

[Full changelog: 0.15.0...0.16.0](https://github.com/exordos/workspace_native/compare/0.15.0...0.16.0)

---

# Workspace Native 0.15.0 - Linux development preview

Two features ported from the reference desktop:

- The right drawer now shows the full channel info screen: a header with the
  channel avatar, purpose text and member count; a three-level notification
  selector (all messages, mentions only, nothing) with optimistic switching;
  the member list with the active member first; and an add-members dialog
  with a searchable candidate picker, selection chips and a footer count.
- Typing @ in the composer opens the reference mention popup: candidates rank
  by frecency, recents and initials/exact/prefix/substring tiers, with
  accented and Cyrillic (e/yo) queries folded; duplicate display names insert
  as "Name (username)"; selected mentions serialize into user links on send
  and the ranking persists across restarts.

Existing installations download available updates automatically and offer
**Install and restart**. For a new per-user installation, extract the archive
and run `bash install.sh`.

[Full changelog: 0.14.0...0.15.0](https://github.com/exordos/workspace_native/compare/0.14.0...0.15.0)

---

# Workspace Native 0.14.0 - Linux development preview

Three features ported from the reference desktop:

- The top bar now hosts the workspace session switcher: quick buttons for up
  to three remembered sessions with unread dots and a "+N" overflow chip, and
  a dropdown listing every remembered account (login plus host and project)
  with a per-organization logout confirmation and an "Add server" row.
- The composer gained the reference formatting toolbar: a collapsible 32px
  action row (link, bold, italic, strikethrough, numbered and bulleted lists,
  quote, code block, inline code, spoiler) with the reference insets and
  separators; the expanded state persists across sessions.
- The right drawer shell matches the reference: a 315px right-docked rounded
  panel with close and back chrome and a layer/screen navigation stack that
  keeps its history across collapse, expansion and stream switches.

Existing installations download available updates automatically and offer
**Install and restart**. For a new per-user installation, extract the archive
and run `bash install.sh`.

[Full changelog: 0.13.2...0.14.0](https://github.com/exordos/workspace_native/compare/0.13.2...0.14.0)

---

# Workspace Native 0.13.2 - Linux development preview

One fix for existing behavior:

- Clicking a desktop notification now also restores and activates the app
  window, matching tray-icon activation: a minimized window is restored and a
  tray-hidden window is shown and focused before the notification's conversation
  opens. Clicks on stale notifications remain a no-op.

Existing installations download available updates automatically and offer
**Install and restart**. For a new per-user installation, extract the archive
and run `bash install.sh`.

[Full changelog: 0.13.1...0.13.2](https://github.com/exordos/workspace_native/compare/0.13.1...0.13.2)

---

# Workspace Native 0.13.1 - Linux development preview

Two fixes for existing behavior:

- Sidebar conversation previews now follow message edits. Editing the last
  message of a channel no longer leaves the stale old text in the sidebar once
  you navigate away, and a deleted last message no longer keeps rendering.
- Long code and exception-trace lines now wrap inside message bubbles instead
  of spilling past the bubble edge.

Existing installations download available updates automatically and offer
**Install and restart**. For a new per-user installation, extract the archive
and run `bash install.sh`.

[Full changelog: 0.13.0...0.13.1](https://github.com/exordos/workspace_native/compare/0.13.0...0.13.1)

---

# Workspace Native 0.13.0 - Linux development preview

The app now tells you how the connection is doing. A banner at the top of the
window shows when you are offline, when the app is reconnecting, or when the
server rate-limits requests, including a live countdown until requests may be
sent again - the most important notice always wins and the banner clears itself
once the connection recovers, including when only the realtime channel comes
back. And if the initial workspace load fails, the same banner offers
**Retry** to load it again without restarting the app (issue #146).

Existing installations download available updates automatically and offer
**Install and restart**. For a new per-user installation, extract the archive
and run `bash install.sh`.

[Full changelog: 0.12.0...0.13.0](https://github.com/exordos/workspace_native/compare/0.12.0...0.13.0)

---

# Workspace Native 0.12.0 - Linux development preview

Long conversations now keep loading upward on their own. Scrolling toward the
top of the loaded messages fetches the next older page of history as soon as
you approach the edge - no need to hunt for the button and page through old
messages one click at a time - and paging stops cleanly at the very beginning
of the history. A page whose messages all belong to another linked account
also self-heals: instead of an empty gap, the view steps past such pages to
the first messages you can actually read (issue #106).

The **Load older messages** and **Load newer messages** buttons stay where
they are and keep working for paging without scrolling.

Existing installations download available updates automatically and offer
**Install and restart**. For a new per-user installation, extract the archive
and run `bash install.sh`.

[Full changelog: 0.11.0...0.12.0](https://github.com/exordos/workspace_native/compare/0.11.0...0.12.0)

---

# Workspace Native 0.11.0 - Linux development preview

Deep links now land where you meant to go. Opening a link to a message buried in
history no longer has to crawl back page by page from the newest message: the
conversation opens centred on the linked message, with the messages that came
after it shown below it and the older ones above. When the loaded window stops
short of the newest message, a **Load newer messages** button appears below the
message list - the mirror of **Load older messages** above it - and paging with
it keeps the highlighted message highlighted until the highlight fades
(issue #71).

Existing installations download available updates automatically and offer
**Install and restart**. For a new per-user installation, extract the archive
and run `bash install.sh`.

[Full changelog: 0.10.0...0.11.0](https://github.com/exordos/workspace_native/compare/0.10.0...0.11.0)

---

# Workspace Native 0.10.0 - Linux development preview

The media viewer arrived in the native client. Clicking a picture in a conversation
now grows it to full size across the window instead of stopping at the inline
thumbnail: on-screen arrows, the keyboard arrow keys or the thumbnail strip at the
bottom move you through a multi-image conversation, a counter shows which picture you
are viewing, a save action writes the full-size file to disk, and Escape or a click on
the dimmed backdrop closes the view. A lone picture opens without the strip or arrows,
and navigation stops at the first and last picture instead of wrapping around (issue #8).

Existing installations download available updates automatically and offer
**Install and restart**. For a new per-user installation, extract the archive
and run `bash install.sh`.

[Full changelog: 0.9.1...0.10.0](https://github.com/exordos/workspace_native/compare/0.9.1...0.10.0)

---

# Workspace Native 0.9.1 - Linux development preview

Editing no longer loses your typing. If you keep typing after pressing Save - or cancel and
reopen the same message while the earlier save is still travelling to the server - the
finishing save no longer wipes the newer text: the message keeps exactly what was saved,
your composer keeps what you typed, and Save comes back for it. A second save of the same
message can no longer race one that is still in progress; it simply waits its turn
(issue #115).

Existing installations download available updates automatically and offer
**Install and restart**. For a new per-user installation, extract the archive
and run `bash install.sh`.

[Full changelog: 0.9.0...0.9.1](https://github.com/exordos/workspace_native/compare/0.9.0...0.9.1)

---

# Workspace Native 0.9.0 - Linux development preview

Replies arrived in the native client. Choosing **Reply** in a message menu opens a
compact quote preview above the composer - the quoted author and a two-line excerpt of
their message - with a **Cancel** action, and sending your answer posts it together
with the quote, so other clients render your message as a reply. Escape closes the
preview without discarding your answer. Clicking the quoted message inside a reply
jumps to its origin: the client opens the conversation that contains it, scrolls the
origin into view and flashes a highlight, the same behaviour the desktop client gives.
Quotes whose origin was deleted show "Message unavailable" in the frame (issue #6).

Existing installations download available updates automatically and offer
**Install and restart**. For a new per-user installation, extract the archive
and run `bash install.sh`.

[Full changelog: 0.8.0...0.9.0](https://github.com/exordos/workspace_native/compare/0.8.0...0.9.0)

---

# Workspace Native 0.8.0 - Linux development preview

Messages are now edited where they are written. Choosing **Edit** in the message menu puts the
message text into the composer under an "Edit message" strip with a **Cancel** action: Enter,
or the send control - which becomes **Save** while an edit is open - saves the new text, and
Escape leaves the edit without saving. Pressing Up in an empty composer edits your own latest
message, the same hotkey the desktop client offers. A draft typed before the edit is left
untouched for the whole session and returns when the edit closes, and a refresh or a dropped
connection can no longer discard text you have already started editing (issue #5).

Existing installations download available updates automatically and offer
**Install and restart**. For a new per-user installation, extract the archive
and run `bash install.sh`.

[Full changelog: 0.7.0...0.8.0](https://github.com/exordos/workspace_native/compare/0.7.0...0.8.0)

---

# Workspace Native 0.7.0 - Linux development preview

The sidebar now previews conversations. Every stream and topic row shows the last
message on one line under its title - the sender's name followed by a short
plain-text summary, with encrypted messages the client cannot open labelled
"Key required". A right-hand column places the unread badge above the last
message's time in the desktop client's relative format: `HH:mm` for today,
"Yesterday", or a compact date such as `Nov 5`. Rows are ordered by their last
message's activity - active conversations first, fully muted streams below -
and rows whose message text is not in memory yet fill themselves in from the
local message cache, updating the row when the text arrives (issue #2).

Existing installations download available updates automatically and offer
**Install and restart**. For a new per-user installation, extract the archive
and run `bash install.sh`.

[Full changelog: 0.6.0...0.7.0](https://github.com/exordos/workspace_native/compare/0.6.0...0.7.0)

---

# Workspace Native 0.6.0 - Linux development preview

The desktop app now raises native desktop notifications. While the window is
in the background, direct messages, @mentions, followed topics and enabled
channel streams raise the operating system's own notification banners, exactly
like the desktop web client: one banner per conversation and author keeps
updating while messages arrive, opening the conversation - or marking the
messages read - dismisses the banner, and a banner click jumps straight to the
conversation. When the window itself is unfocused, the taskbar entry also
asks for attention until you return. Muted conversations and conversations
that are already on screen never notify (issue #3).

Existing installations download available updates automatically and offer
**Install and restart**. For a new per-user installation, extract the archive
and run `bash install.sh`.

[Full changelog: 0.3.16...0.6.0](https://github.com/exordos/workspace_native/compare/0.3.16...0.6.0)

---

# Workspace Native 0.3.16 - Linux development preview

Inbox now recovers from a failed topic-list request. Previously, if a stream's
unread topic list failed to load (a transient server or network error), the
stream stayed stuck on its summary row for the rest of the session: reopening
Inbox never retried, so the unread topics remained invisible until sign-out.
A failed request is now released immediately: closing and reopening Inbox
retries it and the card refines into the per-topic rows. Topic lists that are
still loading stay deduplicated across reopen cycles, and a late failure from
a previous sign-in session can no longer affect the current one (issue #64).

Existing installations download available updates automatically and offer
**Install and restart**. For a new per-user installation, extract the archive
and run `bash install.sh`.

[Full changelog: 0.3.15...0.3.16](https://github.com/exordos/workspace_native/compare/0.3.15...0.3.16)

---

# Workspace Native 0.3.15 - Linux development preview

The login card now opens with organization discovery, mirroring the desktop
web client. You enter the organization's server address first; the app checks
it against the server's public settings, shows the realm name and icon, and
only then mounts the username and password fields - a failed check keeps them
hidden, and editing the address discards a completed check. The server field
suggests organizations previously used on this device (most recent first,
persisted locally), and a packager can preconfigure a default organization
through the WORKSPACE_NATIVE_DEFAULT_LOGIN_ORGANIZATION_URL and
WORKSPACE_NATIVE_DEFAULT_LOGIN_ORGANIZATION_NAME build options (issue #58).

Existing installations download available updates automatically and offer
**Install and restart**. For a new per-user installation, extract the archive
and run `bash install.sh`.

[Full changelog: 0.3.14...0.3.15](https://github.com/exordos/workspace_native/compare/0.3.14...0.3.15)

---

# Workspace Native 0.3.14 - Linux development preview

An Inbox row now follows a topic that moves between streams. Previously a
whole-topic move left the unread row under its old stream - the destination
ignored the move event when it had not already contained that topic - and
clicking the stale row could open the topic at its obsolete location. A
complete topic update now reconciles every Inbox slot: the vacated stream's
card disappears, a not-yet-hydrated destination is refetched so its row shows
the server's counters, and later read and delete events affect the surviving
row wherever the topic now lives (issue #65).

Existing installations download available updates automatically and offer
**Install and restart**. For a new per-user installation, extract the archive
and run `bash install.sh`.

[Full changelog: 0.3.13...0.3.14](https://github.com/exordos/workspace_native/compare/0.3.13...0.3.14)

---

# Workspace Native 0.3.13 - Linux development preview

A message deep link whose target topic is not yet in the loaded topic catalog
now reaches the message. The anchor used to be cancelled while the client
resolved that topic: the topic opened, but the requested message was neither
scrolled to nor highlighted, and the bounded search through older history
never ran. The owned topic-resolution transition now keeps the pending anchor
through its intermediate states, so only unrelated navigation, a superseded
link or sign-out cancel it (issue #55).

Existing installations download available updates automatically and offer
**Install and restart**. For a new per-user installation, extract the archive
and run `bash install.sh`.

[Full changelog: 0.3.12...0.3.13](https://github.com/exordos/workspace_native/compare/0.3.12...0.3.13)

---

# Workspace Native 0.3.12 - Linux development preview

A deleted mention now disappears from Activity as soon as the deletion is
confirmed. Previously a mention that had arrived through the HTTP message
collection stayed on screen - card and preview included, and still clickable -
until the Mentions page was refetched, even though the server had deleted the
message. A confirmed `message.deleted` event now removes the row from the
open All and Unread collections in place without any refetch, covers a
message that is present in both the fetched and the live sources, and a
tombstone keeps an in-flight page response generated before the deletion from
putting the card back. Reading a non-deleted message still keeps its card
(issue #56).

Existing installations download available updates automatically and offer
**Install and restart**. For a new per-user installation, extract the archive
and run `bash install.sh`.

[Full changelog: 0.3.11...0.3.12](https://github.com/exordos/workspace_native/compare/0.3.11...0.3.12)

---

# Workspace Native 0.3.11 - Linux development preview

Sidebar destinations are now mutually exclusive. Opening Drafts, Mentions or
Inbox closes whichever destination was open before, so exactly one page and
one sidebar entry stay selected instead of two pages splitting the content
area, and closing the active page returns directly to the conversation. Draft
contents survive every transition, and both the drafts page keep-alive
lifecycle and the page remount refetches behave exactly as before
(issue #54).

Existing installations download available updates automatically and offer
**Install and restart**. For a new per-user installation, extract the archive
and run `bash install.sh`.

[Full changelog: 0.3.10...0.3.11](https://github.com/exordos/workspace_native/compare/0.3.10...0.3.11)

---

# Workspace Native 0.3.10 - Linux development preview

The sidebar Inbox chip now opens the inbox page. Unread traffic is grouped into
Direct messages and Channels exactly as the reference orders them: every stream
with active unreads becomes a card carrying its aggregate counter, and the card
lists one row per unread topic sorted by last activity, each with its own badge
(counts above 1000 render as "1k+"). A stream whose unread total cannot be
attributed to a topic falls back to a single stream row, and an untitled direct
topic is labelled "All messages". Opening a row navigates to that conversation
and unmounts the page; the close control returns to the previous conversation
without navigating. Realtime read and message events recompute the counters in
place - no polling - and a `/{org}/project/{project}/inbox` deep link opens the
page directly. Muted and passive traffic stays out of the inbox by design, using
active unread counts only (`docs/NATIVE_DESKTOP_INBOX.md`, issue #44).

Existing installations download available updates automatically and offer
**Install and restart**. For a new per-user installation, extract the archive
and run `bash install.sh`.

[Full changelog: 0.3.9...0.3.10](https://github.com/exordos/workspace_native/compare/0.3.9...0.3.10)

---

# Workspace Native 0.3.9 - Linux development preview

The Activity surface gained its Mentions page. The sidebar Mentions chip opens a
list with All and Unread filters, each fetched from the server independently
(`mentioned=true`, newest-first, 50-row pages behind `page_marker` cursors)
with bottom-edge pagination at the reference 64/96 px thresholds. Realtime
events update an open page without refetching: new mentions merge on top, read
events clear only the unread dot, deletions drop the row, and unread counts
come from a dedicated unread-mentions index that falls back to each row's own
read flag while its prefetch walk is still in flight. Unread rows carry the
accent dot and emphasis with channel and topic labels
(`docs/NATIVE_DESKTOP_ACTIVITY_MENTIONS.md`, issue #42).

Existing installations download available updates automatically and offer
**Install and restart**. For a new per-user installation, extract the archive
and run `bash install.sh`.

[Full changelog: 0.3.8...0.3.9](https://github.com/exordos/workspace_native/compare/0.3.8...0.3.9)

---

# Workspace Native 0.3.8 - Linux development preview

Navigation request contracts are now enforced per release instead of
remembered. A warm-navigation benchmark harness drives the verified-cache
navigation window test through a new measurement seam and records exact
request counters - four topic selections reduced to two topic-history GETs,
two stream selections reduced to one aggregate-history and one topic-list
GET, with four cache hits - alongside per-phase timings, externally sampled
peak RSS and a machine-readable report. Request gates are exact; timing and
RSS gates are measured tolerance bands per the issue #16 rule, calibrated
from repeat measurements on the reference stand. The baseline registry, an
approved capture with provenance and a 23-case tooling test ship with the
tree; re-running the scenario takes one command per release and a failed gate
blocks it (`docs/NATIVE_DESKTOP_PERF_BASELINES.md`, issue #13). No
user-visible product behaviour changed.

Existing installations download available updates automatically and offer
**Install and restart**. For a new per-user installation, extract the archive
and run `bash install.sh`.

[Full changelog: 0.3.7...0.3.8](https://github.com/exordos/workspace_native/compare/0.3.7...0.3.8)

---

# Workspace Native 0.3.7 - Linux development preview

Visual parity claims are now mechanical instead of eyeballed. A per-screen
matched-screenshot acceptance harness drives the documented windowed capture
seams for fourteen screens - the conversation scene, message context menu and
reaction picker in light and dark, both saved-history revalidation states, and
all six aggregate-view scenarios - compares every capture with pair-mode
comparison semantics (crop only, never rescale, per-channel mean difference,
changed-pixel count, SHA-256 of every compared byte), and prints a
PASS/FAIL/NOT_RUN verdict per screen with a machine-readable report
(`docs/NATIVE_DESKTOP_SCREENSHOT_ACCEPTANCE.md`, issue #14). Approved baselines
are committed as fixtures; tolerance bands are calibrated from repeat
measurements on the reference stand per the issue #16 rule, and reference-side
slots report NOT_RUN until isolated source fixtures exist. The aggregate
capture seam additionally records the identity/day-divider and read-marker
frames that previously had no screenshot seam. No user-visible product
behaviour changed.

Existing installations download available updates automatically and offer
**Install and restart**. For a new per-user installation, extract the archive
and run `bash install.sh`.

[Full changelog: 0.3.6...0.3.7](https://github.com/exordos/workspace_native/compare/0.3.6...0.3.7)

---

# Workspace Native 0.3.6 - Linux development preview

Secret Service operations no longer wait forever when there is nobody left to
answer. If the desktop login keyring was locked or had never been provisioned
and no prompt provider was registered, the GNOME Keyring daemon opened an
interactive prompt that nothing could answer, and saving or restoring a sign-in
stayed pending indefinitely (issue #28). The session vault now runs a bounded
Secret Service pre-flight before each credential operation: when the default
login collection is missing or locked and no prompt address has an owner, the
operation completes immediately with a definite, actionable error - "The
desktop login keyring is locked. Unlock it to manage saved sign-ins." -
instead of hanging. Every inconclusive probe fails open; with a prompt provider
present the operation is never interrupted, so a real user typing a keyring
password keeps all the time they need, and normal unlocked desktop sessions
behave exactly as before. Three new opt-in acceptance scenarios cover the
unprovisioned profile, a locked profile that recovers after unlocking and a
registered prompt provider suspending the fast-fail
(`docs/NATIVE_DESKTOP_CRYPTO.md`).

Existing installations download available updates automatically and offer
**Install and restart**. For a new per-user installation, extract the archive
and run `bash install.sh`.

[Full changelog: 0.3.5...0.3.6](https://github.com/exordos/workspace_native/compare/0.3.5...0.3.6)

---

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
