# Native login wizard: project step

Status: in progress (issue #60, part of epic #57).
Reference: `workspace_ui` @ `61dacef78dab2f5d0be76f283bebf49fe29b84af` (measured from
source at that sha, not screenshots). Figma: token available; the mockups are
parity-preserving and every value below is quoted from the reference source, so
no mockup-only values are used.

Sibling work: organization discovery step (#58,
[NATIVE_DESKTOP_LOGIN.md](NATIVE_DESKTOP_LOGIN.md)),
[one-time-code challenge](NATIVE_DESKTOP_OTP_LOGIN.md) (#59), registration entry (#61).

## Scope

The project step is entered once the credentials (or OTP) challenge produced a
token and the IAM project list arrived. #60 replaces the native full-width
project buttons (`qml/Main.qml` `loginCard` Repeater `project-` delegates) with
the labelled dropdown, adds the blocking zero-project notice, and enforces the
organization/project embedded in a message deep link before the login commits.

## Reference contract (measured)

Tailwind sizes resolve through the standard scale (`text-sm` = 14 px,
`rounded-lg` = 8 px, `rounded-xl` = 12 px, `gap-4` = 16 px, `h-10` = 40 px,
`px-3` = 12 px, `py-2` = 8 px, `min-h-9` = 36 px, `px-2.5` = 10 px,
`p-1` = 4 px, font-medium = 500).

Step machine (`login-page.ui.tsx:41`): `organization | credentials | otp | project`.

Project form (`login-page-project-form.ui.tsx`):

- Form column gap 16 px (`gap-4`).
- Label `auth.selectProject` ("Project" / "Проект") through `FormField`
  (text-sm font-medium `text-text-primary`, margin-bottom 6 px - measured in #58).
- `Select` (`shared/ui/select/select.ui.tsx` + `shared/ui/listbox/listbox.lib.ts:3-9`):
  trigger `h-10 px-3` (40 x 12 px) `gap-2` rounded-lg border `border-border-subtle`
  `bg-text-field-bg` text-sm `text-text-primary`; placeholder copy
  `auth.selectProjectPlaceholder` ("Select a project" / "Выберите проект")
  rendered in `text-text-muted`; 16 px chevron-down in `text-text-muted`.
  The listbox: rounded-xl, `p-1` (4 px), border `border-subtle`, `bg-bg-elevated`;
  options `min-h-9 px-2.5 py-2` text-sm, active/hover background `bg-card-bg-active`.
  Trigger is disabled while `loading`.
- Option label: `${project.name} — ${project.organizationName}` when
  `organization.organization.name` (trimmed) is non-empty, else `project.name`
  (form lines 46-56; DTO mapping `workspace-auth.lib.ts:509-516`). The separator
  is an em dash U+2014 with spaces (verified with `cat -A` at the sha; the issue
  text prints a plain hyphen - the source value wins).
- Submit button (line 66): `disabled = loading || projectId.length === 0`;
  copy `auth.login` ("Login" / "Войти"), replaced by `auth.loginLoading`
  ("Logging in…" / "Вход…") while pending.
- Zero projects (lines 23-37): only a notice banner - `auth.noProjectsAvailable`
  ("Your account does not have access to any projects in this organization." /
  "У вашего аккаунта нет доступа к проектам этой организации.") in a
  rounded-lg box `px-3 py-2` text-sm, text `notice-base`, border `notice-base`
  at 20 percent, background `notice-base` at 10 percent - plus the same error
  box when `error != null`. No `Select`, no submit control.
- Step description: `auth.projectStepHint` ("Choose the project you want to
  open." / "Выберите проект, который хотите открыть."). The card title does not
  change between wizard steps.

Preselect and enforcement (`login-page.ui.tsx:303-318, 376-392`):

- `enterProjectStep` (after credentials/OTP succeed): when a message link target
  exists and its organization route id differs from the signed-in organization,
  or the project list does not contain the linked project, the page returns to
  the organization step with `messageLinkEntry.wrongAccount`
  ("Sign in to an account for the organization and project in this link." /
  "Войдите в аккаунт организации и проекта, указанных в ссылке.") and does not
  complete the login. Otherwise the step is entered and the selection is
  `messageTarget.projectId ?? projects[0]?.id ?? ""`.
- `handleProjectSubmit`: with a message target, a selection that is not the
  linked project (or a signed-in organization that is not the link's) shows the
  same `wrongAccount` copy and completes nothing. On success
  `completeWorkspaceProjectLogin` runs and the app navigates to
  `resolveWorkspaceLoginRedirect` (`login-redirect.lib.ts`), which only ever
  returns internal `/...` paths: `sanitizeInternalRedirectTarget` trims and
  rejects backslashes, every code point <= 0x20 and 0x7F, anything not starting
  with `/` or starting with `//` (protocol-relative), and legacy messenger
  paths.

Behaviour rows (`login-page.test.tsx`): "loads projects after credentials and
completes login for the selected project" (487), "selects the only available
project by default" (537), "selects the linked project and returns to the
message after login" (552), "keeps the existing session when credentials do not
grant access to the linked project" (570), "shows a blocking empty state when
no projects are available" (581).

## Native mapping

- Data source stays `Session::discoverProjects` (`src/session.cpp`). The IAM
  row's `organization.name` (trimmed) is parsed into the new
  `Project::organizationName` (`src/domain.h`), mirroring
  `workspaceProjectFromIamDto`; `NativeController::projects()` exposes
  `organizationName` for the option label.
- Zero projects is no longer a hard failure: discovery with an empty accessible
  list keeps the token, clears busy and reports the (empty) project list, so the
  wizard can render the reference blocking notice instead of a generic error.
  (This is the one behaviour change inside `Session`.)
- The controller gains the reference `project` step value: when the project
  discovery for a sign-in attempt settles (one, many or zero projects) the step
  becomes `project`; a linked-message guard runs first (below). `backToOrganization`
  from this step drops the not-yet-committed login via `Session::detach()` - the
  saved identity and any previously committed session stay untouched, which is
  the native reading of "keeps the existing session".
- Linked-message guard (`m_pendingDeepLink`): a deep link arriving while the
  wizard is open is parked (`openMessageLink` already defers when
  unauthenticated) and flushed only after a committed session (`flushPendingDeepLink`).
  Before the wizard commits, the parked route's `/org/<org>/project/<project>`
  prefix is compared with `orgRouteIdFromOrigin(server)` and the discovered
  projects: a wrong organization or missing project returns the wizard to the
  organization step with `messageLinkEntry.wrongAccount` and never completes
  the login (`Session::detach()`, prepared token dropped, saved identity kept).
  `openProject` re-checks the guard on submit so a manually re-selected wrong
  project shows the same copy without completing. The post-login navigation
  itself stays internal-only because every parked link is already validated by
  `parseExternalDeepLink`: it rejects backslashes, control characters
  (code points <= 0x20), protocol-relative and `//`-containing routes, encoded
  `%2f/%5c/%00/%0a/%0d` and anything outside the internal route whitelist -
  the native counterpart of `sanitizeInternalRedirectTarget` + internal paths.
- Dropdown surface: new shared control `qml/NativeSelect.qml` styled with the
  measured trigger/listbox geometry (40 px trigger, 8 px radius,
  `bg-text-field-bg`/`border-subtle`, placeholder in `text-text-muted`, 16 px
  chevron; options 36 px min-height with `bg-card-bg-active` highlight).
  Login card keeps object names: label `projectStepLabel`, combo
  `projectSelect`, options `projectOption-<uuid>`, submit `projectLoginButton`,
  notice banner `noProjectsBanner`, error banner `projectErrorBanner`.
  Selection preselects the linked project when present, else the first row.
  Submit is disabled until a selection exists and shows `Logging in…` while the
  session is refreshing. The project step gets the wizard's "Organization" affordance
  (back to the organization step, mirroring the reference back row).
- The legacy `project-` button Repeater and the "Back to sign in" button leave
  the card; the title no longer flips to "Workspace"/"Choose a project" (the
  reference title is step-independent, and that state is now the project step).

## Deviations

- The issue text prints the option separator as "name - organizationName"; the
  reference source at the cited sha uses the em dash " - " (U+2014). The source
  measurement wins (mirrored in tests through the reference test strings
  "Customer support - Example organization" style).
- The reference "wrong account" rejection keeps the *prepared* login in memory
  (page-local state) and simply leaves it unusable; the native client drops the
  in-memory token of the rejected attempt via `Session::detach()` because a
  live native token must not idle in an unauthenticated wizard. Saved vault
  identities and any already-committed session are kept, satisfying the
  "keeps the existing session" row.
- Zero native projects previously produced the error "No accessible Workspace
  projects were returned." and dropped the token; per the reference it is now a
  settled empty list rendered as the blocking notice. Requests that fail
  transport or return a malformed payload still fail as before.

## Tests

- `native-native-session` (`tests/session-test.cpp`): zero/one/many projects
  against the mock server - many with mixed organization payloads
  (`organization.name` present, blank, absent), one default project, zero
  settling without an error signal.
- `native-native-window` (`tests/native-window-test.cpp`): dropdown geometry and
  copy in light/dark and en/ru; single-project preselect + submit; option label
  with/without organization; blocking empty state without any action control;
  linked-project preselect and submit enforcement with the `wrongAccount` copy;
  linked wrong-organization rejection on entry back to the organization step.
  Existing startup/recovery rows that clicked the removed `project-` buttons now
  submit the preselected project through `projectLoginButton`.
