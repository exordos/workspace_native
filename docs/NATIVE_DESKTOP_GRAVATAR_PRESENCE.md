# Gravatar avatars and live presence on all surfaces (issue #4)

Figma token unavailable - mockups not consulted. The reference implementation in
the pinned `workspace_ui` checkout (sha `75bd21f`) is the design oracle for
every fact below.

## Reference contract (measured at 75bd21f)

`packages/web/src/shared/lib/workspace-avatar-urn.lib.ts` resolves an avatar urn
into exactly one source:

| urn form | resolution |
| --- | --- |
| `urn:image:<uuid>` | Workspace file download (`kind: "file"`, `loadWorkspaceAvatar`, bounded Map cache: `MAX_CACHED_WORKSPACE_AVATARS = 64` entries / 32 MiB, consumer-counted leases). |
| `urn:gravatar:<hash>` (`^[0-9a-f]{32}$`, case-insensitive) | `https://secure.gravatar.com/avatar/<hash>?d=identicon&s=500` (`resolveGravatarUrl`, lowercased hash). |
| `urn:gavatar:<uuid>` | `https://secure.gravatar.com/avatar/<uuid-without-dashes-lowercased>?d=identicon&s=500` (`resolveGeneratedAvatarUrl`). |
| `urn:url:<http(s) url>` | direct external `<img src>`, only http/https and only without userinfo. |
| anything else | `null` - the Avatar shows its initials fallback. |

The gravatar urn itself is produced by `buildWorkspaceDefaultAvatarUrn(email,
userUuid)` = `urn:gravatar:` + MD5 of the trimmed lowercased email (MD5 is only
the Gravatar address format), falling back to the user uuid. `WorkspaceAvatar`
(`packages/web/src/features/workspace-avatar/workspace-avatar.ui.tsx`) renders
the resolved url through the shared `Avatar` circle, letters underneath while a
photo is absent or failing.

`docs/PRESENCE_AND_STATUS.md` plus `entities/user/user-workspace-presence-reporter.lib.ts`
define the presence half: a single `POST /users/{uuid}/actions/presence/invoke`
heartbeat carrying the measured presence and the deliberately chosen status, on
the cadence the native client already mirrors in `src/native-controller.cpp`
(`PresenceActiveMs = 30000`, `PresenceUnfocusedMs = 120000`,
`PresenceUnfocusedBatteryMs = 160000`, `PresenceHiddenMs = 300000`,
`PresenceBatteryMultiplier = 2`, issue #78).

## Native state today

Live presence is already implemented on every cited surface:

- heartbeat cadence + battery widening (`src/native-controller.cpp`,
  `reportPresenceHeartbeat`, `presenceHeartbeatIntervalMs`);
- server-row presence projection with the `updated_at` ordering witness and the
  active/idle-only mapping (`NativeController::makeUserPresentation`);
- `PresenceBadge` in `qml/Main.qml` on the account rail (`presence-viewer`),
  sidebar direct rows (`presence-stream-<id>`), the DM header
  (`presence-direct-header`) and grouped message avatars
  (`presence-author-<id>`), plus `NativeUserProfile`, `NativeChannelInfo`,
  `NativeEncryptedChatInfo` and the mention row.

The gap is the avatar half: `imageAvatarReference` (`src/domain.cpp`) only
accepts `urn:image:<uuid>`, so `urn:gravatar:`/`urn:gavatar:` values from the
server are stripped to empty at parse time. Every real account without an
uploaded photo therefore renders initials-only on every surface, while the
reference shows its Gravatar (or identicon) image.

## Design

Extend the avatar pipeline to the two Gravatar urn forms; keep everything else
untouched.

1. `src/domain.{h,cpp}` - add `AvatarSource` + `resolveAvatarSource(urn)` and
   `participantAvatarReference(value)`, mirroring `resolveWorkspaceAvatarSource`
   and the table above for `urn:image`/`urn:gravatar`/`urn:gavatar` (trim via
   `sourceTrimmed`, hash lowercased). `participantAvatarReference` keeps the
   exact sanitization role `imageAvatarReference` plays today (validated urn
   passes through, anything else becomes empty), and is used at the same call
   sites: the `makeUserPresentation` avatar field, the two directory raw
   projections in `src/native-controller.cpp`, and the names cache row in
   `src/native-cache.cpp`. `imageAvatarReference` stays as the file-only helper
   (`AvatarImages`/uploads keep using it).
2. `src/avatar-images.{h,cpp}` - `AvatarImages` resolves each acquired urn via
   `resolveAvatarSource`. `File` entries behave exactly as today
   (`FileClient::downloadBytes`, authorization stays inside `ApiClient`).
   `Gravatar` entries are fetched by a dedicated `QNetworkAccessManager` that
   sends no credentials, follows redirects, and only ever targets the exact
   `https://secure.gravatar.com` origin (mirrored on `allowLoopbackHttpForTests`
   exactly like `ApiClient`, so QtTest can serve fixtures from the loopback
   server without any developer path). Responses are capped at 8 MiB
   (`MaximumRemoteBytes`, well under the reference's 32 MiB file-cache budget)
   and then enter the existing shared decode lane: same `thumbnail()`
   center-crop/circular mask, same 64-entry/128 px bounds, same lease rules,
   same transport-failure retry-on-next-acquire, same scope fences. Entry keys
   are `g:<hash>` for gravatar sources and the lowercased file uuid for files,
   so the two kinds can never collide in the shared image map.
3. Deferred, deliberately: `urn:url:<external>` passthrough (arbitrary-URL
   fetch from a native process is outside the bounded-cache trust model and the
   server never emits it today; the reference only gets it for free because the
   browser sandbox does the containment), the status-message dialog behaviors
   already covered by issues #78/#177, and Qt-level Gravatar disk persistence
   (the manager cache is intentionally in-memory and bounded).

## Tests

`packages/native/tests/avatar-images-test.cpp` gains QtTest cases: the gravatar
urn fetches `/avatar/<32hex>?d=identicon&s=500` with no authorization header,
decodes through the same masked 128 px thumbnail, deduplicates across two
consumers, rejects malformed gravatar/gavatar urns without any network work,
and gets the same transport-failure retry semantics as file avatars.
`native-controller-test.cpp` grows an avatar-passthrough witness proving a
`urn:gravatar:` server value survives the projection to `account()`/
`directPeer()`/message rows instead of being stripped, using the existing
`AvatarReferenceFixture`. Geometry and color facts cited above name
`workspace_ui@75bd21f` files; no new visible geometry is introduced - the photo
enters the circles that already exist.
