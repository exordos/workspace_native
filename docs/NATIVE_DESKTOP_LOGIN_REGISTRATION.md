# Native login wizard: registration entry

Status: implemented (issue #61, part of epic #57).
Reference: `workspace_ui` @ `61dacef78dab2f5d0be76f283bebf49fe29b84af` (measured from
source at that sha, not screenshots). Figma token unavailable - mockups not
consulted; the code oracle is the sole visual source, and every value below is
quoted from the reference source.

Sibling work: organization discovery step (#58,
[NATIVE_DESKTOP_LOGIN.md](NATIVE_DESKTOP_LOGIN.md)),
[one-time-code challenge](NATIVE_DESKTOP_OTP_LOGIN.md) (#59),
[project step](NATIVE_DESKTOP_LOGIN_PROJECT_STEP.md) (#60).

## Scope

Issue #61 adds the registration entry to the credentials step of the login
wizard: the `No account?` hint plus a full-width `Register` button, rendered
only when the organization's public `server_settings` payload advertises an
https registration URL. Clicking opens the URL in the system default browser.
The https-only acceptance rule lives where the server settings are parsed, so
every consumer sees a normalized value and no consumer can re-introduce an
unsafe scheme.

## Reference contract (measured)

Rendering (`packages/web/src/pages/login/login-page.ui.tsx`, step
`credentials`, lines 512-538):

- Line 524: the block renders only when `serverSettings?.registrationUrl != null`
  (`registrationUrl` is already the normalized nullable value; see below).
- Line 525: container `flex flex-col gap-2 text-center text-sm text-text-muted`
  (`gap-2` = 8 px, `text-sm` = 14 px, muted token).
- Line 526: hint paragraph `t("auth.registrationHint")`.
- Lines 527-532: anchor `href={serverSettings.registrationUrl}` with
  `target="_blank" rel="noopener noreferrer"` and classes
  `hover:bg-bg-elevated/60 inline-flex h-9 w-full items-center justify-center
  rounded-lg border border-border-subtle bg-bg-elevated px-4 font-medium
  text-text-primary transition-colors` (`h-9` = 36 px, `rounded-lg` = 8 px,
  `px-4` = 16 px, font-medium = 500; hover background `bg-bg-elevated` at
  60 percent opacity).
- Line 533: copy `t("auth.register")`.

The value is produced once during organization discovery
(`login-page.ui.tsx:156`): `registrationUrl:
normalizeLoginRegistrationUrl(data.registration_url)`.

Normalization (`packages/web/src/pages/login/login-registration-url.lib.ts`,
11 lines): trim; empty -> `null`; parse with `new URL()`; keep the value only
when `url.protocol === "https:"` (returning the WHATWG-normalized `url.href`);
parse failure or any other protocol -> `null`. Measured truth table (Node's
WHATWG parser at the reference checkout):

| advertised value | kept? |
| --- | --- |
| `https://reg.example/signup` | yes (`href` identical) |
| ` https://reg.example/signup ` (outer whitespace) | yes, trimmed |
| `HTTPS://Reg.Example/X` | yes -> `https://reg.example/X` (case normalized) |
| `` (empty) / whitespace-only | no (throws / empty) |
| `http://reg.example` | no (protocol `http:`) |
| `javascript:alert(1)` | no (protocol `javascript:`) |
| `not a url` / `reg.example/signup` | no (throws: no scheme) |
| `https://` / `https:/` | no (throws: empty host) |
| `https:/reg.example/signup` | yes -> `https://reg.example/signup` (WHATWG slash recovery) |
| `https:///x` | yes -> `https://x/` (WHATWG empty-authority recovery) |

Behaviour contract (`packages/web/src/pages/login/login-page.test.tsx`):

- Line 363: "shows registration only when server settings provide a safe URL" -
  with `registration_url: "https://iam.example.com/register"` the credentials
  step shows a link named "Register" with exactly that href and `target=_blank`.
- Line 377: "hides registration when server settings omit the URL" - no "No
  account?" text, no Register link.

Copy (measured at the sha, `auth` namespace, en.json/ru.json lines 113-114):

- `auth.registrationHint`: "No account?" / "Нет аккаунта?"
- `auth.register`: "Register" / "Зарегистрироваться"

External opening: the web page is a plain `target="_blank"` anchor; the Electron
shell (`packages/electron/src/main.ts:411-425`) routes denied external
navigations and new-window requests through `isSafeExternalUrl` +
`shell.openExternal`. The native client has no embedded login webview, so the
system-browser handoff is the direct `QDesktopServices::openUrl` pattern already
used for external Markdown links (`packages/native/src/native-markdown.cpp:341`,
exposed to QML as `Qt.openUrlExternally`, precedent `qml/Main.qml:9330`).

## Native mapping

### Normalization at the parse site

`GET /api/workspace/v1/messenger/server_settings` is parsed in
`src/session.cpp` `Session::fetchOrganizationSettings` (lines 377-402; #58
kept the three rendered fields verbatim and #58's groundwork guarded the scheme
with a `startsWith("https://")` check in `src/native-controller.cpp:10238-10241`).
Issue #61 moves the rule to the single parse site: `Session` stores
`registration_url` only as a normalized value (empty string when the advertised
value must not be rendered), and the controller copies that value into the
QML-facing `organizationSettings.registrationUrl` key without re-judging it.

`normalizeLoginRegistrationUrl` (static, `src/session.cpp`) mirrors the
reference: trim; empty -> null; strict `QUrl` parse; scheme compare to `https`
case-insensitively; require a non-empty host; otherwise null. Measured
deviations, all fail-closed (block hidden, nothing opened) relative to the
WHATWG parser's leniency:

- `https:/reg.example/signup` and `https:///x`: WHATWG recovers the authority
  and keeps the URL; the strict `QUrl` parse has an empty host, so native hides
  the block.
- Backslash variants (`https:\reg.example\signup`): WHATWG treats backslashes
  as slashes for special schemes; native hides the block.
- The reference stores the WHATWG-normalized `href` (scheme/host lowercased,
  trailing slash added); native keeps the advertised string trimmed exactly as
  advertised. Both open the same resource, and the trimmed raw value avoids
  re-serializing userinfo/percent-encoding differently from the server's intent.
  The observable href equality of the reference behaviour test
  (`login-page.test.tsx:363-374`, an already-normalized input) holds either way.

The acceptance-relevant categories (`https` kept; `http`, `javascript`,
garbage, empty rejected) are identical to the reference.

### Credentials step presentation

`qml/Main.qml` `loginCard` -> `credentialsFormLoader` sourceComponent: after
`signInButton`, a `registrationBlock` `ColumnLayout` (spacing 8,
`Layout.fillWidth`) is visible only when
`(root.controller.organizationSettings.registrationUrl || "").length > 0`
(the map key is absent before discovery succeeds, hence the `|| ""` guard):

- `registrationHintText`: `qsTr("No account?")`, 14 px, `text-muted`, centered.
- `registerButton`: `NativeButton`, full width, `implicitHeight: 36`, custom
  background matching the measured anchor (`radius: 8`, border `border-subtle`
  width 1, `bg-elevated`, hover `Qt.rgba(bg-elevated, 0.6)`), content text
  `qsTr("Register")` 14 px font-medium `text-primary`. `Accessible.name` is
  the same copy.
- `onClicked`: `Qt.openUrlExternally(root.controller.organizationSettings.registrationUrl)`
  - the system default browser handoff (same binding the Markdown external-link
  handler uses; `Qt.openUrlExternally` is the QML binding of
  `QDesktopServices::openUrl`). `property string url` mirrors the reference
  `href` for test assertion; the window case group asserts presence/absence and
  the wired URL (clicking a real browser handoff is not assertable headless).
- Loader lifetime: the block is inside the existing `credentialsFormLoader`,
  so it appears only on the credentials step (never on organization, otp or
  project steps), mirroring `step === "credentials"` gating. `ComponentBehavior:
  Bound` is file-global in `Main.qml`; the block adds no delegate and refers to
  `root` lexically like the surrounding form items.

The default (en) strings are the `qsTr` sources; the Russian copies
("Нет аккаунта?" / "Зарегистрироваться") go to the reviewed catalog
`packages/native/translations/workspace_ru.ts` under context `Main`
(`native-native-locale` fails on any source string missing from the catalog or
any unfinished translation).

## Tests (TDD vertical slices)

1. RED->GREEN: `native-session` (`tests/session-test.cpp`, the CTest id CMake
   registers for the session group the acceptance names "normalization cases"):
   data-driven `loginRegistrationUrlNormalization` asserts the truth table above
   through `Session::fetchOrganizationSettings` against the mock server -
   https kept (and trimmed), missing/empty/whitespace/http/javascript/garbage/
   empty-host normalized to the empty string; a non-string JSON value is
   dropped. The controller pass-through stays covered by the existing
   `loginWizardDiscoveryGatesCredentials` href-equality case.
2. RED->GREEN: `native-native-window` (`tests/native-window-test.cpp`),
   `loginRegistrationEntryFollowsServerSettings` data rows - advertised https
   (en and ru), omitted field, `http://` value, `javascript:` value. Present
   rows: block visible, hint + button copy per locale (ru rows also pin the
   catalog delivery), `url` property equals the advertised URL, button height
   36 and full width (equal to the credentials fields). Absent rows: the
   credentials form is shown but neither `registrationBlock` nor
   `registerButton` exists. Cases live inside `class NativeWindowTest` before
   its closing brace; the group runs the whole binary (no per-case CMake list).
3. GREEN: `native-native-locale` - catalog completeness/argument guards with the
   two new `Main` messages.

`native-native-session` names the session test group in the issue text; the
registered CTest ids are `native-session` (binary `session-test`) and
`native-native-window` (binary `native-window-test`) per
`packages/native/CMakeLists.txt` (`add_test(NAME native-${test} ...)`).

Design fidelity case: parity-preserving.
