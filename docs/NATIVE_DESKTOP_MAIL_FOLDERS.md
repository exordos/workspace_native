# Native Mail folders, messages and previews

The desktop Mail section reads the active account's folders and messages. It
opens the Inbox when available and retains the selected folder across refreshes.
The reader displays a local-time date, recipients, plain text and file references.

## Workspace Mail API

The client displays Workspace folders from the selected project. Messages must
also match the selected folder and its linked account. The server can omit
`external_account_uuid` for a local row because the JSON serializer skips null
values. Rows with unusable UUIDs are skipped before
deduplication. Selecting a folder uses the IDs from the loaded mailbox.

The client requests `/api/workspace/v1/mail/folders/` and
`/api/workspace/v1/mail/messages/` through the authenticated session. It follows
`X-Pagination-Marker` using `page_marker` and filters messages by the selected
`folder_uuid`. A repeated marker or authorization error produces a retryable error.
An empty folder collection produces an empty mailbox. A real response from the
folders endpoint keeps all message requests on the Workspace API.

The reader prefers text body parts. For HTML-only messages it converts the body
to plain text locally with resource loading disabled. File URNs are shown as
plain-text references. In a live mailbox, the reader can mark messages read or
unread, change their star, move them to another folder of the same external
account, and move them to or restore them from Trash. The controls wait for
the server response and refresh the mailbox. A rejected operation leaves the
message in place and displays its error. The preview keeps the actions disabled.
Draft persistence and queued delivery are tracked in #852.

## Reference contract (workspace_ui e0194ff)

Measured from source at the pinned sha:

- `packages/web/src/pages/mail/mail-page.ui.tsx` - the shipping desktop
  `MailPage` is an `EmbedFrame` over a configured `VITE_MAIL_EMBED_URL` or the
  same-origin fallback `embeds/mail-placeholder.html`; when the embed is not
  allowed it renders an unavailable card (Icon `mail` size 64 at `opacity-50`,
  heading `nav.mail` `text-xl font-medium text-text-primary`, body
  `app.webModeUnavailable` `text-sm text-text-muted`, gap 16, p-24).
- `packages/web/public/embeds/mail-placeholder.html` - the actual mailbox mock
  embedded by the desktop product. Layout numbers (CSS px at 1x):
  - shell: `min(1120px, 100%)`, radius 16, 1 px border `#d9e2f0`, panel bg
    `#ffffff`, page bg `#f4f7fc`, body padding 18.
  - header: padding 12/18/16, `h1` 23 px, search box min-width 260, radius 10,
    padding 8/12, 13 px, bg `#f8faff`, text `#64738d`.
  - layout: 3-column grid `220px 1fr 1fr`, min-height 470, column separator
    1 px `#d9e2f0`.
  - folder rows: radius 9, padding 8/10, margin-bottom 6, 13 px, text
    `#364866`; active: bg `#eaf0ff`, border `#d2dcff`, text `#2347a7`, weight
    600. Sidebar bg `#fbfcff`, padding 14.
  - thread cards: radius 10, padding 10/12, margin-bottom 8, 1 px border
    `#d9e2f0`; active bg `#f4f7ff` border `#cdd9ff`; sender 12 px `#5b6c89`;
    subject 14 px weight 600; snippet 12 px `#64738d`; threads column padding 10.
  - reader column: padding 16; label 12 px `#64738d` margin-bottom 8; title
    18 px margin-bottom 10; body `#3c4d6c` line-height 1.45.
- The placeholder lists Inbox (12), Sent, Drafts, Archive, Spam; the issue
  contract fixes the folder set at six folders
  Inbox/Sent/Drafts/Archive/Spam/Trash, each with an unread count.
- Strings: `nav.mail` = "Mail" / "Почта"
  (`packages/web/src/i18n/locales/{en,ru}.json:447`). Every new visible string
  ships in both locales and is added to
  `packages/native/translations/workspace_ru.ts`.

Palette mapping: the placeholder HTML carries fixed demo hex values. Native
maps them onto the app palette tokens (measured from
`packages/native/qml/NativeTheme.qml`): page/panel -> `bg` / `bg-elevated`,
separators -> `border-subtle`, selection -> `accent-soft` + `accent`,
row rest -> `card-bg` / active `card-bg-active`, text -> `text` /
`text-muted`. This keeps light + dark parity with the rest of the client
instead of freezing the placeholder's light-only hexes; deviation is allowed
here because this is brand-new functionality (#851 UI phase).

## Reader reference

The opened-message and selection frames in the Exordos Core file
(`18687:30325` and `18687:31139`) show the subject above the sender, recipients
and date, followed by the message text and actions. In the native mock mailbox
the reader occupies its existing column. The test fixture exercises an empty
selection and a long selected message in English and Russian in both color
modes. The API integration in #852 owns delivery and write actions.

## Compose

Compose opens a fresh local draft from the folder rail. The modal follows the
centered compose frame `18687:30635` with recipients, subject, attachments,
a plain-text body and a disabled Send button. Moving past an empty recipient
field displays the validation error until a recipient is supplied. The user
can select local files or remove their rows. The store reads the display name
and formatted byte size; the draft holds no paths or file contents and makes
no network requests. Closing the dialog, leaving Mail or retiring the session
discards its fields and attachments. The Mail test group opens and closes the
dialog and exercises validation, local attachment metadata and owner retirement
in both themes and languages.

## Figma

`FIGMA_TOKEN` returns HTTP 403 from this host as of 2026-11-26 - mockups not
consulted; the node id recorded by the issue is Mailbox and folders
`18705-46468`. Geometry above is taken from the reference source instead.

## Native design

- C++ `MailStore` (`packages/native/src/mail-store.{h,cpp}`), exposed through
  the existing `controller.mail` property: `folders` (list of
  `{id, title, unread}`), `folderId`, `messages` (list of
  `{uuid, sender, subject, snippet, time, unread}`), `state`
  (`loading|ready|error`), `refresh()` and `selectFolder(id)`. The older preview adapter issues
  `GET /mail/folders` and `GET /mail/messages?folder=<id>` through the
  session `ApiClient` (token renewal Cancel; unauthenticated callers never see
  the page because the rail entry only exists inside a session).
- The store retires its folder list, rows, selection and pending requests
  when the account, server or project changes. It keeps loaded rows on
  same-owner credential renewal. The active Mail route fetches for its new owner.
- QML `NativeMailPage.qml` replaces `mailSectionPage`
  (`packages/native/qml/Main.qml:13244` block); the rail entry
  `sectionMailButton` (Main.qml:3098) and `controller.section == "mail"`
  routing share the existing desktop shell. Selecting a row opens
  `NativeMailReader.qml` with the subject, sender, optional list of recipients,
  supplied display date and plain-text body. The reader accepts the optional
  adapter fields `recipients` (array of names/addresses), `date` (display
  string) and `body` (plain text). Older summaries use their existing `time`
  and show that the full body is unavailable. Long headers wrap and the pane
  scrolls vertically. Reply, forward, read-state and deletion controls are
  visibly disabled pending the live contract. Changing folders clears selection.
- States: `loading` (spinner rows), `ready`, `empty` folder state per folder,
  `error` with a retry control re-invoking `refresh()`. A deployment that does
  not serve the Workspace or preview endpoints (HTTP 404) falls
  back to the sample mailbox compiled into the client and reaches `ready`:
  opening Mail in a production build shows the promised folders instead of a
  dead error screen (fix round, exordy finding 4712325987). Real transport and
  server failures (non-404) keep the `error` + retry path. Sample mode is armed
  solely by the folders response: if `/mail/folders` answered `200`, a `404` on
  `/mail/messages` stays `error` with an empty list, so sample rows can never be
  listed under server-provided folders (exordy finding 4712650373).
- Mail identifies built-in folders, unread counts and messages with a persistent
  localized notice below the header. If only the folders come from the sample,
  the notice identifies only the folder data as examples.
- All server-supplied strings (folder title, message rows, reader headers and body) render with `textFormat: Text.PlainText`, so HTML-shaped
  content can never be interpreted as rich text or request remote images
  (exordy finding 4712325992).
- Fixtures: window tests answer the two endpoints from a deterministic mock
  scenario (six folders, Inbox unread 12 matching the placeholder, three
  inbox messages matching the placeholder senders/subjects/snippets, one
  empty folder, one error folder). The compiled-in sample mailbox mirrors the
  window-test fixtures so both paths render the same contract.

## Test plan

- New CTest group `native-native-mail` (`tests/native-mail-test.cpp`): folder
  list order + unread counts from fixtures; selecting a folder reloads the
  message list; row selection marks the row and drives the reader placeholder;
  loading -> ready; empty folder state; error state + retry re-fetch reaches
  ready; ru + en label rendering.
- `native-window-test` cases: `mailRailButtonOpensMockMailbox` (click
  `sectionMailButton` -> mail page visible, unavailable card gone) and
  `mailFolderSelectionShowsFixtureMessages`; bucket registration via
  `gen-test-buckets.py` after measuring the new cases solo.
- Acceptance = every bullet of issue #879 proven by the named tests; screenshot
  pair light + dark attached to the PR.

Mailbox events use the shared Workspace cursor. The open mailbox refreshes
from the service after a change or reconnect. A hidden mailbox refreshes
when it is next opened. A surviving message stays selected, and a deleted
message closes its reader. These collection refreshes leave the local
compose draft intact.
