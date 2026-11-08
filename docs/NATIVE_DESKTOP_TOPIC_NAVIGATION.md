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

## Verification scope

The native regression coverage includes exact request paths, canonical placement,
retained list completeness, draft preservation, no global catalog reloads,
supersession at both resolver stages, and reentrant callbacks. A visible Qt window
test hit-tests and clicks actual message links for known, unknown, moved,
failed and superseded routes. The input signal must be observed before route and
request-count assertions.

These local fixtures verify the implemented client behavior. They do not establish
that a deployed backend resolves arbitrary deleted or merged UUIDs. The client
does not invent an alias table, alternate endpoint or provider fallback. Browser
back/forward history, message-anchor navigation and live service acceptance remain
separate parity requirements.
