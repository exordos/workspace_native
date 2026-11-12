# Composer mention suggestions (issue #123)

Ports the reference composer `@mention` suggestion popup to the native client:
trigger detection, conversation-context ranking, row presentation, keyboard
selection, insert semantics, canonical URN serialization on send, and persisted
frecency. Everything below is measured from the pinned reference checkout
`workspace_ui` at sha `61dacef78dab2f5d0be76f283bebf49fe29b84af`; no value is
copied from a screenshot.

## Trigger (message-composer.ui.tsx:959-1012, detectMention)

- Applied on every text change and after caret-moving keys (the reference calls
  `onDetectMention(value, cursor)` from change handlers; native re-evaluates on
  text change and after ArrowLeft/ArrowRight/Home/End re-evaluate the span
  before the caret).
- Regex over `text.slice(0, cursor)`: `(?:^|[\s([{,.:;!?])(@)(\S*)$`.
  The trigger is an `@` at start-of-text or after a space or one of
  ``([{,.:;!?`` ; the query is the non-space run after it. An `@` inside a word
  or email (`hello@a`) matches nothing and closes the popup.
- `startPos = cursor - query.length - 1`. Opening resets the active row to 0.
- Closing clears query, start position and active index.

## Ranking (features/mention-suggest/mention-suggest.lib.ts,
message-composer-mention-context.lib.ts:14-135)

Match tiers (lower wins): exact username/displayName = 0, prefix = 1, word
prefix (name words, username words, email local part) or multi-token prefix = 2,
substring of username/displayName = 3, substring of email = 4, substring of the
substring of the lowercased user UUID only for queries >= 4 chars = 5. An empty query keeps
everybody in tier 0. The reference folds case and strips combining marks after
`normalize("NFD")` (mention-suggest.lib.ts `normalizeMentionText`), which folds
Cyrillic `yo` (U+0451 decomposes to U+0455 U+0308) into `ye` and Latin
`e-acute` into `e`. Qt stores text precomposed and has no NFD transform, so the
native port folds case, trims, and maps `yo`/`YO` to `ye`/`YE` explicitly; a
full combining-mark fold is not reproduced. The reference suites only assert the
`yo`/`ye` direction of this folding for non-ASCII text (the URN tests pin the
literal Cyrillic names, message-composer-body.lib.test.ts), and native tests cover
that.
Tie order inside a tier: context score descending, then shorter display name
(only for a non-empty query), then display name locale order, then UUID order.

Context scores added per user (mention-suggest.lib.ts constants):

- recent author at index i (< 20): `+4000 - 100*i`
- channel member: `+1500`
- DM partner at index i (< 20): `+1200 - 40*i`
- frecency: `+min(score, 10) * 60`
- self: `-5000`

At most `MENTION_SUGGESTION_LIMIT = 8` rows are returned.

Context snapshot (message-composer-mention-context.lib.ts:14-135):

- Recent authors: scan the open conversation window from the tail over at most
  the last 200 messages (`RECENT_MESSAGE_SCAN_DEPTH = 200`), first-seen wins,
  keep at most 20 (`RECENT_AUTHOR_LIMIT`).
- Channel members: from stream bindings of the stream being composed in, but
  only while `streamBindingsLoadedByStreamId[stream] === true`; unknown
  membership yields `null` and marks nothing (the unloaded-membership state).
- DM partners: every stream with a direct peer, ordered by the referenced last
  message's `createdAt` (fallback `updatedAt`) descending, capped at 20.

Native gap record: the native controller loads stream bindings only through the
encrypted-chat authority and the channel-creation check; an ordinary channel
selection fetches no membership, so the native context reports membership as
unknown there (the reference's unloaded-membership state: no `outside channel`
chip, membership never negative). Membership ranking is exercised through the
ranking API fixtures and the encrypted-authority source.

## Suggestion rows (message-composer-mention-dropdown.ui.tsx:27-121)

- Container: width 320 (`w-80`), max content height 192 (`max-h-48`), 4px gap
  above the composer input (`bottom-full mb-1`), 8px radius (`rounded-lg`),
  1px subtle border, elevated background, vertical scroll.
- Row: `px-3 py-2` (12px/8px), 8px column gap (`gap-2`), 14px primary text
  (`text-sm`), active row uses the base background (`bg-bg`).
- Avatar: `WorkspaceAvatar size="sm"` = 32px circle, 12px initials
  (`w-8 h-8 text-xs`, shared/ui/avatar/avatar.tsx:8), first character of the
  display name (fallback first char of the username, then `?`).
- Presence dot: 8px (`h-2 w-2`, sm in presence-indicator.tsx SIZE_MAP) with a
  2px background ring; colors active green, idle orange, offline muted
  (COLOR_MAP). `do_not_disturb` folds to idle (`resolveUserPresenceVisual`,
  user-selectors.lib.ts:28-41); native folds it at the model layer already.
- Secondary line: `@username` when a username exists, otherwise the email;
  11px secondary text (`text-[11px]`).
- `outside channel` chip only when `outsideChannel === true`: 10px text
  (`text-[10px]`), `px-1.5 py-0.5` (6px/2px), localized label
  `composer.mentionOutsideChannel`: en `not in channel`
  (en.json:1546); the reviewed Russian catalog carries the ru.json:1558 value.
- Empty list shows one muted row `search.noResults`: en `No results found`
  (en.json:1288); the reviewed Russian catalog carries the ru.json:1300 value.

## Popup keyboard (message-composer-write-body-keydown.lib.ts:39-67)

While the popup is open and rows exist: ArrowDown increments and ArrowUp
decrements the active index, both clamped at the ends (no wrap); Enter without
Shift selects the active row instead of sending; Escape closes the popup and
is swallowed. Send with the popup open first closes the popup (the reference
hides the dropdown in `handleSendOrNewline` before sending).

## Insert semantics (message-composer.ui.tsx:1006-1034,
message-composer-body.lib.ts:149-176)

Selecting replaces `value[startPos, cursor)` with `@<DisplayName> ` and puts
the caret after the trailing space (`insertWorkspaceMention`). Inserting records
one frecency pick for the user (`recordMentionPick`, the pick on select is the
only frecency signal), closes the popup and restores focus/caret after the name.

## Send serialization (message-composer-body.lib.ts:149-176 and :189-193)

At send time the visible draft converts to canonical mention URNs:

- Mention entries carry `userUuid`, `displayName` and `visibleText`. When more
  than one selectable user shares a display name, every such user's
  `visibleText` becomes `"<DisplayName> (<username>)"` (message-composer.ui.tsx
  displayNameCounts); a bare `@DisplayName` for an ambiguous name therefore
  matches nothing and stays literal (skipped as ambiguous).
- Pattern: `(^|[^\p{L}\p{N}_.-])@(alt)` with alternatives ordered longest
  visibleText first and the negative boundary `(?![\p{L}\p{N}_.-])`; each hit
  becomes `(<prefix>)[<displayName>](urn:user:<userUuid>)`. Email addresses
  keep their literal text because `.`/word characters block the boundary.
- Serialization happens on the send body only (before the reply quote prefix
  join and trimming, mirroring `buildOutgoingMessageBody`); the draft keeps the
  visible names.
- Encrypted channels are excluded: native blocks plaintext composer sends
  there, matching `mentionsSupported` gating in the reference composer.

## Frecency (features/mention-suggest/mention-frecency.lib.ts)

- Per record: `count`, `lastUsedAt`; a pick folds the previous count through
  `count * 2^(-ageMs / HALF_LIFE)` (HALF_LIFE = 14 days) and adds 1.
- Reads expose per-user decayed scores; entries decayed below 0.05 drop; at
  most 200 entries kept (sorted by decayed score).
- Native stores this in `NativePreferences` scoped to the controller's
  `creationOwnerKey()` (the opaque 64-hex account identity that also scopes the
  draft store). It is deliberately not `ownerKey()`: that HTTPS-vault identity
  is empty on loopback deployments and test fixtures, and the reference store
  is origin+account scoped regardless of transport. The controller passes the
  scope per call (`recordMentionPick(uuid, creationOwnerKey())`), so ranking
  reads and writes always partition by the same key. Stored under the
  `mentions/owners/<owner>` settings group, so it survives restarts and never
  leaks across owners. Losing the store only costs ranking quality.

## Native surface

- `NativeController::rankMentionSuggestions(query, candidates, context)`:
  pure static ranking with the constants above.
- `NativeController::mentionSuggestions(query)`: QML-facing list built from the
  project user directory (`channelMemberChoices` data: id/displayName/
  username/email plus presentation presence/avatar), with the live context
  (self, recent authors from the loaded window, DM partners from the stream
  catalog, membership when known, frecency from preferences).
- `NativeController::recordMentionPick(userUuid)`: owner-guarded pick write.
- `NativePreferences::recordMentionPick(userUuid, owner)` and
  `mentionFrecencyFor(owner)`.
- `workspace::serializeComposerMentions(value, mentions)` and
  `workspace::buildWorkspaceUserMention(name, uuid)` in `src/domain.cpp`
  used by the ordinary send path.
- QML: `composerMentionPopup` in `Main.qml`, positioned above the composer
  input with the `composerEmojiPicker` pattern. The rows are a positional
  `Repeater` over a `Column` inside the popup's `Flickable` (the session
  switcher pattern): delegates read live candidates through the popup id, so
  a refreshed suggestion list can never dangle a delegate. Popup content is
  parented visually only, so window tests walk the visual tree
  (`childItems()`) instead of `findChild` to assert rows.

## Test mapping

- `native-native-controller`: ranking fixtures (members/recent/authors order,
  self penalty, tiers, limit), URN serialization (emails skipped, ambiguous
  names skipped, punctuation boundaries, Cyrillic), frecency persistence and
  restart order, live-scene `mentionSuggestions` ordering, send wire check.
- `native-native-window`: typing `@` opens the popup, refinement, Escape
  closes, `@` inside a word never opens it, ArrowDown+Enter inserts
  `@Name ` with the caret after the name and sends the URN on the next Enter,
  restart (fresh `NativePreferences` on the same file) keeps the frecency pick.
