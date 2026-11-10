# Native login wizard: organization discovery step

Status: implemented (issue #58, part of epic #57).
Reference: `workspace_ui` @ `61dacef78dab2f5d0be76f283bebf49fe29b84af` (measured from
source, not screenshots). Tailwind sizes resolve through the standard scale
(`text-sm` = 14 px, `text-xl` = 20 px, `rounded-lg` = 8 px, `gap-4` = 16 px,
`gap-6` = 24 px, `gap-2` = 8 px, `mt-1` = 4 px, `h-12 w-12` = 48 px,
`max-w-md` = 448 px, `p-4` = 16 px, `px-4 py-3` = 16/12 px, `py-2.5` = 10 px,
`px-3 py-2` = 12/8 px, `h-[18px] w-[18px]` = 18 px, font-medium = 500,
font-semibold = 600).

## Scope

Issue #58 delivers the login step wizard shell plus the **organization discovery
step**: the server-address field with persisted organization history, the public
`server_settings` discovery request, the realm preview card, the default-organization
shortcut, and the credential gate (credentials and the sign-in action stay hidden
until discovery for the current address succeeded).

Out of scope, owned by sibling issues: the one-time-code step as its own wizard
step (#59), registration entry (#61), the project-step dropdown restyle (#60).
Until #59 lands, the native credentials step keeps the existing inline
"one-time code, if required" field so OTP logins do not regress; the reference
only asks for the code after IAM rejects the password attempt, which is exactly
what #59 will reproduce.

## Reference contract (measured)

Step machine (`packages/web/src/pages/login/login-page.ui.tsx:41`):
`organization | credentials | otp | project`. Initial step: `organization`.

Login column (`login-page.ui.tsx:433-453`): page `bg-bg p-4` centered column,
`max-w-md` (448 px max width), gaps 24 px. Back row is text-sm muted with a
16 px chevron rotated 180 (lines 435-449), label `auth.organization`
("Organization" / "Организация") while a later step is shown. Title block is
centered: title text-xl (20 px) font-semibold `text-text-primary`, switching
between `auth.connectToZulip` ("Connect to server" / "Подключение к серверу")
and `auth.addServer` ("Add server" / "Добавить сервер") once saved sessions
exist (line 423); description mt-1 (4 px) text-sm `text-text-muted`, per-step
key `auth.organizationStepHint` ("Enter your organization address first." /
"Сначала укажите адрес вашей организации.").

Deviation note (mapping, not restyle): the reference back row on the
organization step navigates to the session-list home route. The native client
has no separate home route - the saved-accounts list lives inside the login
card - so the back affordance is rendered only from the credentials step,
where it has an action (return to the organization step). Everything else is
reproduced exactly.

Organization step form (lines 464-510), column gap 16 px:
- FormField label `auth.serverUrl` ("Server address" / "Адрес сервера");
  label style: text-sm font-medium `text-text-primary`, margin-bottom 6 px
  (`shared/ui/form-field/form-field.ui.tsx`).
- `InputSelector` (searchable combobox) with placeholder
  `auth.zulipServerUrlHint` ("https://chat.example.com", both locales);
  options are `readWorkspaceOrganizationHistory()` origins; searchable filter
  is a case-insensitive substring of the current text over label+value
  (`shared/ui/input-selector/input-selector.ui.tsx:45-51`).
- Discovery fires on blur when the trimmed value is a valid realm URL
  (lines 229-233) and on Next (lines 250-285). Invalid Next shows
  `auth.invalidServerUrl` ("Server address must be a valid HTTPS URL" /
  "Адрес сервера должен быть валидным HTTPS URL"). Stale responses are dropped
  by a fetch-id race guard (lines 121, 144-188). Editing the field to a value
  different from the checked one cancels the pending advance and returns to the
  organization step (lines 207-227).
- Next button is disabled unless the trimmed text is a valid realm URL; while
  discovery is in flight its copy becomes `auth.organizationStepLoading`
  ("Loading organization settings…" / "Загружаем настройки организации…").
- Error banner (`auth.organizationSettingsLoadError`: "Could not load
  organization settings. Check the server address and try again." / "Не удалось
  загрузить настройки организации. Проверьте адрес сервера и попробуйте ещё
  раз."): rounded-lg (8 px) border px-3 py-2 text-sm, text `notice-base`,
  border `notice-base` at 20 percent, background `notice-base` at 10 percent.
  Credentials stay hidden when discovery failed (test `login-page.test.tsx:470`).

Discovery request: `fetchWorkspaceServerSettingsForOrganization`
(`entities/workspace-auth/workspace-auth.lib.ts:554-565`) resolves the input to
an origin and issues the public messenger call `GET
{origin}/api/workspace/v1/messenger/server_settings` with **no bearer auth**
(`shared/api/messenger-transport.internal.ts:156` "Public endpoints, for example
server_settings, must not receive bearer auth"). Fields consumed:
`realm_name`, `realm_icon`, `registration_url`. Native parsing accepts any 2xx
JSON object and reads those three members as optional strings (the React DTO
validates the full messenger payload; the native surface only renders the
three fields, so a truncated success payload stays usable rather than being
flashed as an error). A non-object body or transport error is a failure.

Realm preview (`login-page-realm-preview.ui.tsx`): shown when settings exist
and `realm_name` or `realm_icon` is non-empty. Card: rounded-lg border
`border-subtle` `bg-bg-elevated`, padding 16/12 px, vertical column gap 8 px,
centered; logo 48 x 48 rounded-lg object-contain; realm name text-sm
font-medium `text-text-primary`. Broken logo falls back to
`organization-fallback.svg` (loaded once, no retry loop). Realm-relative icons
resolve against the organization origin; same-origin icons load on the desktop
build (`login-page-icon-url.lib.ts`). `registration_url` is kept as an https-only
value (`login-registration-url.lib.ts`) for #61 and is not rendered by this issue.

Default organization shortcut (lines 126-129, 482-499): rendered only when the
build defines `DEFAULT_LOGIN_ORGANIZATION_URL`. Block rounded-lg border
`border-subtle` `bg-bg-elevated` px-4 py-3 column gap 8 px; hint
`auth.defaultOrganizationHint` ("Or connect to our public service." / "Или
можете подключиться к нашему публичному сервису.") text-sm muted; button row
gap 8 px with an 18 x 18 rounded object-contain organization logo; button
itself rounded-lg border `border-subtle` `bg-bg` px-4 py-2.5 (16/10 px)
text-sm font-medium `text-text-primary`. Pressing it clears all downstream
state and refills the field (lines 235-248). Name falls back to
`auth.defaultOrganizationName` ("Exordos Core Public", both locales).
Native build default: none compiled in (block hidden); the value can also come
from `WORKSPACE_NATIVE_DEFAULT_LOGIN_ORGANIZATION_URL` /
`WORKSPACE_NATIVE_DEFAULT_LOGIN_ORGANIZATION_NAME` process environment, which
keeps the surface testable without a source edit.

Organization history (`entities/workspace-auth/workspace-organization-history.lib.ts`):
persisted origin list, deduped, newest first, recorded only after a login
committed a session. Native stores it in `NativePreferences` under the
`login/organizationHistory` key as normalized origins.

Realm URL validation mirrors `shared/lib/validation.ts` (`isValidRealmUrl`,
`hasValidHostnameLabels`): absolute URL, `https` scheme, hostname without
spaces, non-empty labels, no leading/trailing/double dots. The native API
transport additionally requires a bare origin (no path/query/fragment/userinfo);
the wizard therefore normalizes any valid input to its origin before discovery
and sign-in, mirroring the reference `new URL(input).origin` behaviour. Test
harnesses keep the established loopback-HTTP allowance of `ApiClient`.

Initial auto-advance (`login-initial-organization.lib.ts`): in the desktop build
there is no browser origin, so only a message-link organization resolves and
auto-advances. The native client has no message-link login entry yet (deep links
route inside an authenticated session), so the initial step is always the
organization step with an empty field. When #73 (deep-link navigation) grows a
pre-auth entry, the auto-advance belongs there.

## Native design

- `NativePreferences`: `organizationHistory` property + `organizationHistoryValue`
  round-trip through `login/organizationHistory`; `recordOrganizationOrigin(origin)`
  dedupes and prepends; origin normalization drops path/query/fragment.
- `NativeController` owns the wizard state machine (mirrors the React states
  `realm`, `checkedRealm`, `serverSettings`, `settingsLoading`, `fetchIdRef`,
  `pendingAuthRealmRef`):
  - properties: `loginStep` ("organization"/"credentials"),
    `organizationRealm`, `organizationLoading`, `organizationSettings`
    (map with `organizationOrigin`, `realmName`, `realmIconUrl`,
    `registrationUrl`), `defaultOrganizationUrl`, `defaultOrganizationName`,
    `organizationHistoryReady` (not used in QML; history binds through
    preferences).
  - invokables: `organizationInputChanged(text)` (invalidates a stale check the
    same way `handleRealmChange` does), `discoverOrganization(text)` (blur
    path), `continueFromOrganization(text)` (Next path), `backToOrganization()`,
    `selectDefaultOrganization()`, `isOrganizationUrlValid(text)` (mirrors
    `isValidRealmUrl` with the transport origin rules applied).
  - a monotonic attempt counter drops stale discovery replies; the request runs
    over `Session::fetchOrganizationSettings(origin, callback)` which sets the
    transport origin and issues the public GET through the existing guarded
    request path.
  - `setPreferences(NativePreferences*)` lets the app (and tests) attach the
    store; on `Session::authenticated` the current transport origin is recorded
    into the history.
- `Main.qml` `loginCard`: rebuilt to the measured column. The credential block
  is inside a `Loader`, so before discovery succeeds the user, password and OTP
  items are not instantiated at all. New objectNames: `loginTitle`,
  `loginDescription`, `loginBackButton`, `organizationForm`,
  `organizationNextButton`, `organizationErrorBanner`,
  `organizationHistoryPopup`, `organizationHistoryOption-<i>`,
  `defaultOrganizationBlock`, `defaultOrganizationButton`,
  `realmPreviewCard`, `realmPreviewIcon`, `realmPreviewName`. Existing names
  `serverInput`, `userInput`, `passwordInput`, `otpInput`, `signInButton` are
  kept for the fields they still name.
- Fallback logo: `qml/organization-fallback.svg`, copied from
  `workspace_ui/packages/web/public/organization-fallback.svg`.
- New user-visible `qsTr` strings get entries in
  `packages/native/translations/workspace_ru.ts` with the exact reference ru copy.

## Test matrix

`native-session` (`tests/session-test.cpp`):
1. `organizationSettingsRequestIsPublic` - success path issues
   `GET /api/workspace/v1/messenger/server_settings` with the public flag and
   without an Authorization header, and returns the three realm fields.
2. `organizationSettingsRejectsInvalidResponses` - transport failure and a
   non-object body both fail without touching session auth state.

`native-controller` (`tests/native-controller-test.cpp`):
3. `loginWizardStartsOnOrganizationStep` - initial step, Next with an invalid
   value shows the invalid-URL copy, no request issued.
4. `loginWizardDiscoveryGatesCredentials` - blur discovery fills the settings
   map and the realm icon resolves to an absolute URL, but only Next after a
   completed check reveals credentials; Next while loading defers the advance.
5. `loginWizardFailureKeepsCredentialsHidden` - server error keeps
   `loginStep == organization`, shows the load-error copy, and repeating Next
   re-shows the error without a second request.
6. `loginWizardStaleDiscoveryIsIgnored` - a deferred response from a superseded
   attempt never lands after a newer attempt completed.
7. `loginWizardRealmEditInvalidatesCheck` - editing after success resets the
   step, clears settings, and the next Next refetches.
8. `defaultOrganizationShortcutFillsRealmAndClearsState` - property hidden by
   default; with the env override the shortcut refills the field and clears the
   checked settings and any error.
9. `loginRecordsOrganizationHistory` - a completed sign-in persists the origin;
   a fresh controller on the same store sees it (dedup, newest first).

`native-preferences` (`tests/native-preferences-test.cpp`):
10. `organizationHistoryRoundTrip` - normalization, dedupe, ordering, restart.

`native-window` (`tests/native-window-test.cpp`):
11. `loginCardMountsOrganizationStepOnly` - title/description/field mounted;
    `userInput`/`passwordInput` absent until credentials reveal; Next disabled
    while the field is empty; history popup lists persisted origins and choosing
    one fills the field and triggers discovery (reference tests 154/162/180).
12. `loginCardShowsRealmPreviewAndBlocksOnFailure` - success shows the preview
    with the realm name; failure keeps credentials hidden and shows the banner
    (reference test 470).
13. Existing card-driven cases repointed: focus/IME probes use `serverInput`;
    the resume-busy assertion checks `serverInput` enabled state; the single
    full card sign-in now passes through the organization step and asserts the
    origin lands in the persisted history.

Reference behaviour tests mirrored from `login-page.test.tsx`: 154 (organization
field only on first step), 162 (persisted suggestions after remount), 180
(history fills without submitting), 221 (default button independent of history),
470 (credentials hidden on discovery failure).

## Geometry and color oracles

All values in this document are measured from the reference sha above. Window
tests assert structural facts (mounted/hidden, exact copy, token-resolved
colors via the palette maps) and avoid host font-metric pixel oracles per the
rule in issue #16.
