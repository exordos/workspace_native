# Vendored Jitsi External API script

`jitsi-external_api.js` is a verbatim copy of `https://meet.jit.si/external_api.js`
fetched 2026-11-12, Apache License 2.0 (Jitsi Meet). It is vendored here only to
exercise the reference "vendored script loaded on demand from our own base url"
contract (workspace_ui `jitsi-external-api.loader.ts`) inside the spike.
The throwaway spike is not shipped; production vendoring is a call-issue task.
