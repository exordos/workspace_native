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
