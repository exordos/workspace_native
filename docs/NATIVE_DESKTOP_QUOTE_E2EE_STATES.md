# Native desktop decrypt-aware quote states for encrypted sources

Issue: #222. Branch lane: `cassi/reply-e2ee-quote-states`.

Reference contract measured from the read-only `workspace_ui` checkout at sha
`230e02b8b0273dee556a406746d122180a68b3e7` (paths below are relative to
`packages/web/src/` in that tree). This release extends the shipped quote frame
(`docs/NATIVE_DESKTOP_REPLIES.md`) so a reply that quotes an end-to-end
encrypted origin renders the same four decryption sub-states the reference
bubble pipeline renders for encrypted content, instead of collapsing to
"Message unavailable".

## What the reference actually does

`widgets/workspace-message-list/workspace-message-quote.ui.tsx`:

- `:49-52` `DECRYPTION_TITLES = { error: "e2ee.messageDecryptError",
  "missing-key": "e2ee.lockedTitle" }`.
- `:123-143` `resolveQuotePresentation` decryption branch (evaluated before
  the selected-text and rendered-markdown branches):
  - `status === "preparing"` -> header keeps the author label, **no body**
    (neither placeholder nor text is rendered).
  - `status === "missing-key"` -> `EncryptedMessagePlaceholder state="locked"`
    titled `t("e2ee.lockedTitle")` ("Key required" / "Нужен ключ").
  - `status === "error"` -> placeholder `state="error"` titled
    `t("e2ee.messageDecryptError")` ("Could not decrypt message") with one
    inline ghost action `t("e2ee.messageDecryptRetry")` ("Retry"),
    `onClick: retry` (the per-message decrypt retry) and
    `disabled: decryption.retryPending` (`:124,137`).
  - `status === "ready"` falls through to the markdown branches.
- `:180-201` encrypted sources route through the presentation hook: if the
  resolved store message is `payload.kind === "e2ee"` and no chat-prepared
  presentation exists yet, `LocalWorkspaceMessageQuote` runs
  `useMessagePresentation(source)` - the same per-message decrypt state
  machine the bubble body uses - and feeds it in as `decryption`.
- `:226` `sourceLocked = encryptedSource && decryption?.status !== "ready"`
  suppresses document parsing of the quote body while locked.
- `:252-256` ready substitution: `sourceMarkdown` (snapshot or markdown
  payload) is replaced by `decryption.markdown` when the decryption reached
  `ready` - the decrypted text renders with the same quote-stripped render
  options (`QUOTE_RENDER_OPTIONS`, `:42-48`: no code copy, no protected
  media, no attachments, no gallery).
- Header label stays the author display name in all decryption sub-states
  (`resolveQuoteAuthorLabel`, `:54-63`); only `unavailable` swaps it for the
  muted "Message unavailable".

`features/encrypted-chat-message/encrypted-message-placeholder.ui.tsx`
(the lane brief names it `shared/ui/encrypted-message-placeholder.ui.tsx`; at
the pinned sha it lives under `features/encrypted-chat-message/` — geometry,
pixel oracle):

- Container: `w-[min(30rem,60vw)] max-w-full` -> width `min(480 px, 60 vw)`,
  capped at the parent width. `role="alert"` for error, `role="status"`
  otherwise.
- Locked: `sr-only` title + bar block `h-9 gap-2 blur-[6px]` -> height
  36 px, 8 px gap, 6 px blur; two bars `h-2 w-[92%]` and `h-2 w-[65%]`
  `rounded-full bg-text-muted/35` -> bar height 8 px, radii 4 px+ (pill),
  colors `text-muted` at 35 % alpha (`#707070` dark / `#989898` light,
  `shared/styles/themes/blue-cold.ts`), vertical layout centered (native
  bubble uses bar `y = 6` and `y = 22`, i.e. 6 + 8 + 8 + 8 + 6 = 36).
- Error: `flex min-h-9 flex-wrap items-center gap-x-2 gap-y-1 text-sm
  text-text-muted` -> min-height 36 px, 8 px horizontal / 4 px vertical gap,
  14 px text in `text-muted`; `warning` icon size 16; the Retry is a ghost
  sm Button with `!h-auto !rounded-sm !p-0 underline underline-offset-2`
  riding the 20 px line, disabled at 50 % opacity.

The quote frame chrome itself (message surface: `bg` at 35 % alpha, radius
6 px, 2 px accent left bar `#7087ff`, padding 8 x / 6 y, 12 px / 16 px
weight-500 accent header, 14 px / 20 px body) is unchanged from
`docs/NATIVE_DESKTOP_REPLIES.md`; the decryption sub-states only replace the
body area.

## Figma (design intent)

Figma token available. Tree refreshed for file `Q3BtN9qwfQV8rAsyBAgCy1`; page
"design макеты (desktop)" (151:2290), section "цитирование" `13944:37360`
("citation / multicitata") inspected via the nodes API (depth 4). Its frames
are `13591:37413`, `13796:9276` (own message with a multicitata),
`13591:37456`, `13796:9307` (message with a citation), `13591:37482`,
`13796:9210` (message with a multicitata), `13591:37364`, `13796:9350` (own
message with a citation) and `COMPONENT_SET 13591:37568`
("citation/multicitata", variants `13591:37567` / `13591:37566`). **The
quotation section carries no locked/error quote-frame variant**: the encrypted
message mockups live in the separate sections "Шифрованные чаты"
(10879:26030) and "Защищенный канал (с ключом шифрования)" (13127:36323) at
bubble level only. The best matching frame, "message with a citation"
`13591:37456`, is committed at `docs/design/222-figma-mockup.png` (PNG 708 x
416, scale 2). The mockup renders the ordinary citation frame (left bar +
translucent fill + accent author header + white body); its orange accent tint
is mockup-only - the shipped reference code pins the accent to `#7087ff` and
the message-surface fill to 35 % alpha, so per the lane contract the
**reference code remains the pixel oracle**, and the new locked/error
sub-state geometry is mirrored from the shipped native encrypted bubble rows
(which are themselves the verified pixel copy of the reference placeholder).

## Native gap (measured at ba40b84)

- `src/native-controller.cpp` row projection (`messageRows`, quote block
  ~:9040-9064): the loaded-window branch accepts `origin->kind == "markdown"`
  only; an e2ee origin falls through to the resolved-quote store lookup.
- `scheduleQuoteFetch` (~:11698): a fetched DTO whose `kind != "markdown"`
  is stored as `QuoteRecord::missing`, so a quoted encrypted message renders
  the muted "Message unavailable" - the reference never does this for a
  resolvable encrypted origin.
- `qml/Main.qml` quote frame `message-quote-<id>` (~:4604-4717) knows only
  loading / ready / unavailable.
- The bubble already ships the exact sub-state rows this issue mirrors:
  `encrypted-placeholder-<id>` (Main.qml ~:4772-4789, 36 px, bars at y 6/22,
  `text-muted` 35 %, `MultiEffect` blur, accessible titles "Key required" /
  "Decrypting message") and `encrypted-error-<id>` (~:4790-4817, 36 px min
  row, warning glyph 16, "Could not decrypt message", ghost
  `encrypted-retry-<id>` wired to `controller.retryMessageDecrypt(id)`,
  disabled while `decryptRetryPendingUuids` contains the uuid - issue #221).

## Native design

### Controller (`src/native-controller.cpp` / `.h`)

Quote resolution in the row projection gains an encrypted-origin branch that
reuses the chat-prepared decryption state (`m_chatPrepared`), mirroring
`encryptionState` (:9025-9032) but projected for the **origin**:

`quoteStatus` vocabulary becomes `"" | loading | preparing | locked | error |
ready | unavailable` (backward compatible: the existing values and all
markdown behavior are untouched).

For a message whose leading line is a quote reference with origin `id`:

1. Loaded markdown origin -> unchanged (`ready`, author + raw markdown).
2. Loaded e2ee origin -> decrypt-aware:
   - author name always resolves (`quoteAuthorName`, fallback = reference
     label, else "Quote") because the reference keeps the author header in
     every decryption sub-state;
   - `preparedCurrent = canReadChat && m_chatPrepared(id) && hash current`:
     - not current -> `preparing` (author header, no body; matches
       reference `:125`);
     - `Present` -> `ready`, `quoteContent` = decrypted text (the ready
       substitution at reference `:252-256`);
     - `Missing` -> `locked` (no key on this device);
     - `Error` -> `error` (key present but decrypt failed).
   - `!canReadChat` (vault unavailable / chat not readable) -> `locked`,
     matching the bubble treatment where "Key required" is the fallback
     placeholder title.
3. Store hit from `GET /messages/?uuid=`: unchanged for markdown
   (`ready`/`unavailable`); an e2ee DTO is no longer cached as an
   authoritative miss - `QuoteRecord` gains `bool decrypt` and the row
   renders `locked` (see Deviations D1). A transport failure remains
   retriable with the existing 3 s backoff + self-heal timer (unchanged);
   only an empty/invalid answer stays `missing`.
4. Unknown origin -> `loading` + one deduplicated lookup (unchanged).

The single-message retry path stays the controller's shipped
`retryMessageDecrypt(uuid)` (#221): it accepts exactly an e2ee row in the
current encrypted conversation whose prepared entry is present-current and
failed, which is precisely the `error` quote state for a loaded origin.
`decryptRetryPendingUuids` is the shared in-flight signal, so a quote-frame
Retry and a bubble Retry for the same message can never double-decrypt.

Cache consistency is unchanged: confirmed payload mutations refresh cached
quote previews and confirmed deletions drop them (PR #119 round); e2ee store
records carry no content, so only the delete-drop applies to them and the
loaded-window branch always wins over the store record, letting an origin
that later scrolls into the window upgrade `locked -> preparing -> ready`.

### QML (`qml/Main.qml`)

Inside `message-quote-<id>` (gated out while the quoting bubble itself is
locked), after the header text:

- `quote-locked-<id>`: locked placeholder mirroring `encrypted-placeholder-*`
  exactly - width `Math.min(480, root.width * 0.6, parent.width)`, height
  36 px, two `text-muted` bars at 35 % opacity (92 % / 65 % width, height 8,
  radius 4, y 6 / 22), clipped `MultiEffect` blur layer, `Accessible.name`
  = "Key required" (the reference title is `sr-only`, the bars are
  `aria-hidden`). Visible while `quoteStatus === "locked"`.
- `quote-error-<id>`: row mirroring `encrypted-error-*` - min height 36 px,
  8 px spacing, warning glyph 16 px, "Could not decrypt message" 14 px
  `text-muted` (`Accessible.AlertMessage`), plus `quote-retry-<id>`
  (`InlineDecryptRetryAction`, text "Retry") calling
  `controller.retryMessageDecrypt(quoteId)`, disabled while
  `decryptRetryPendingUuids` contains the quote id. Visible while
  `quoteStatus === "error"`.
- `preparing` renders the author header with no body (the existing loading /
  selection / body texts keep requiring their own statuses, so nothing
  renders).

The existing frame MouseArea keeps click-to-jump; the Retry button sits above
it in the content column and consumes its own clicks (sibling order), so the
inline action never navigates.

### i18n

No new strings. The reused sources "Key required", "Could not decrypt
message" and "Retry" already exist in
`packages/native/translations/workspace_ru.ts` (:797, :769, :401 group ->
"Нужен ключ" / "Не удалось расшифровать" / "Повторить"); the locale guard
stays green without catalog edits.

## Deviations from Source (named and justified)

- **D1 - out-of-window e2ee origin**: a quoted origin fetched by
  `GET /messages/?uuid=` that is not part of a loaded conversation window
  renders the locked "Key required" placeholder instead of being decrypted
  locally. The reference's `useMessagePresentation` runs its presentation
  pipeline over the store record; the native single-message decrypt state is
  by contract the chat-prepared `m_chatPrepared` entry (issue scope: "reuse
  chat-prepared decryption state"), which exists only for loaded windows.
  The moment the origin is actually loaded (opening its conversation or
  jumping to it), the loaded-window branch takes precedence and the state
  machine resolves normally.
- **D2 - `preparing` visibility**: the native transient (history loaded,
  slice not yet run) is not separately observable in tests; it shares the
  author-header-no-body rendering contract with the reference and is
  exercised only as the pre-first-slice state.
- **D3 - unreadable chat**: with no readable conversation the placeholder
  title is "Key required" (bubble precedent), where the reference title for
  its equivalent state also falls back to the locked title.

## Acceptance and tests

Controller (core, `tests/native-e2ee-read-test.cpp`, `native-native-e2ee-read`):

- quoted e2ee origin with no key -> `quoteStatus "locked"`, author header
  kept, no quote body;
- key arrives for the origin generation -> `ready` + decrypted
  `quoteContent` (and the quoting reply itself decrypts independently of
  origin state);
- wrong key material for the origin generation -> `error`, and
  `retryMessageDecrypt(origin)` runs the per-message retry (in-flight
  membership, repair -> `ready`);
- origin missing from history: transport failure keeps the lookup retriable
  (no cached miss); an e2ee lookup answer marks `decrypt` (renders `locked`),
  an empty answer still marks `unavailable`;
- existing markdown quote/reply expectations unchanged (native-controller
  quote/reply functions keep passing).

Window (`native-native-e2ee-ui`, display `:36`):

- quoted e2ee message in an opened encrypted conversation: locked bars
  (`quote-locked-*`: 36 px height, bars, `Accessible.name` "Key required")
  before the origin key resolves; decrypted quote text after the key lands;
- no-key origin conversation keeps the "Key required" bars and never reveals
  plaintext;
- forced decrypt error shows `quote-error-*` + `quote-retry-*`; clicking
  Retry re-runs the per-message decrypt path (disabled while in flight), and
  repairing the key replaces the row body with the decrypted text;
- markdown quote cases in `native-window-test` stay green (the ordinary
  quote frame is byte-for-byte unchanged).

Locale: `native-native-locale` guard unchanged (no new strings).
