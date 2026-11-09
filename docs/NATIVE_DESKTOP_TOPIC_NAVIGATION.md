# Native topic navigation

Topic links in rendered messages use the same native controller route as explicit
topic opening. A topic that is already in the verified current list opens without
a resolver request. An old channel hint in the link does not override the known
placement.

For an unknown topic, the controller requests the exact topic detail endpoint.
The owned returned DTO supplies its canonical topic and channel UUIDs. When that
channel is absent from the current catalog, one exact channel detail request
obtains its complete metadata. The resulting selection opens the canonical topic
directly, without first fetching aggregate channel history or reloading the global
channel and user catalogs.

The topic detail response initially supplies only one retained topic. That seed
allows its history to be read while the complete topic list loads; it does not
authorize message sending or replace the complete cached list. The authoritative
list can update or remove the seed. A failed list remains retryable through normal
topic selection.

The latest explicit navigation wins. A later known or unknown topic selection,
account/API generation change, cache replacement or controller destruction cancels
the pending resolver. Request handles cancel only the exact in-flight request;
they cannot cancel another client or a completed/replacement request. A selected
topic follows an owned realtime move without an additional resolver lookup.

An admitted deletion of the selected explicit topic keeps that address and its
local draft while the same resolver asks for a canonical placement. The retired
body and write authority are cleared before outward callbacks. A valid owned
result opens directly; 404 or invalid ownership leaves an empty, locked explicit
route. A newer selection or logout wins, and poll/live messages cannot repopulate
the unresolved route. This automatic event path does not change the separate
explicit refresh/reloadSnapshot missing-list fallback or introduce an owner-wide
topic collection.

## Following renames, moves and merges

A whole-topic rename or cross-stream move arrives as one `topic.updated` event
carrying the full canonical topic snapshot for every stream user; individual
messages are not replayed. The topic keeps its canonical UUID, so loaded history,
cached pages and window membership stay addressed by that UUID. A pure rename
inside the same stream therefore changes only topic metadata: the selected
window keeps its messages and pagination cursors untouched and no message
request is issued.

A cross-stream move of the selected topic removes the retired row from the local
topic list first, so the fast path can never serve the former placement, and
then opens the canonical UUID and stream directly from the owned event DTO. The
vacated conversation is a definitive membership loss, not an unverified change:
its History bucket (the topic pages and the former stream aggregate pages) is
invalidated, mirroring the deleted-conversation rule, so a revisited former
route cannot hydrate messages that no longer live there. The destination route
keeps its saved pages as stale offline content and revalidates normally. A
selection that is not the moved topic only loses the retired row; unrelated
loaded windows and their cached pages are untouched. Repeated renames of the
same topic are idempotent: each event re-seeds the same canonical UUID and
neither duplicates nor drops messages or topic rows.

A single-message move between topics or streams arrives as `message.updated`
with the identity preserved. The selected window admits the message only when
it fits the loaded pagination boundary and otherwise retires it from the former
window; the history projection evicts the saved row from every page that does
not belong to the new placement and admits it into a saved destination head page
only when the page ordering stays coherent. Message pages are never
manufactured, and the retained owned body keeps the moved payload so the
destination can disclose it later without loading a destination page.

Links follow the canonical identity only. A stored topic reference resolves to
the owned DTO's canonical UUID and stream (with an owned stream detail fallback
when the catalog lacks it), never to a display name; every resolver stage is
cancellable by a newer selection, route change, owner replacement or account
generation change. Because a resolver ticket is pinned to the exact route it
started on, an in-flight topic lookup can never republish a different route and
therefore cannot take control of a staged direct-message anchor or any other
concurrent selection; a superseded ticket publishes nothing.

## Verification scope

The native regression coverage includes exact request paths, canonical placement,
retained list completeness, draft preservation, no global catalog reloads,
supersession at both resolver stages, and reentrant callbacks. A visible Qt window
test hit-tests and clicks actual message links for known, unknown, moved,
failed and superseded routes. The input signal must be observed before route and
request-count assertions. A whole-topic move regression additionally asserts that
the canonical open follows the event DTO, the vacated topic and former-stream
aggregate cache buckets are gone while unrelated cached windows keep their
pages, the followed window's messages stay in place without any request to the
vacated route, and a rename storm over the receiving stream neither duplicates
messages or topic rows nor issues further history reads.

These local fixtures verify the implemented client behavior. They do not establish
that a deployed backend resolves arbitrary deleted or merged UUIDs. The client
does not invent an alias table, alternate endpoint or provider fallback. Browser
back/forward history, message-anchor navigation and live service acceptance remain
separate parity requirements.
