# Auth idle timeout enforcement (native)

Issue #165 enforces the "sign me out after inactivity" setting in the native
client. The settings row itself belongs to issue #135; this document covers the
enforcement half: the inactivity deadline, its activity sources, the pause
while no window is shown, live preset reaction, and the credential wipe on
expiry.

## Reference contract (pinned web source, sha 61dacef78dab2f5d0be76f283bebf49fe29b84af)

| Concern | Reference location | Behavior |
| --- | --- | --- |
| Preset -> ms | `packages/web/src/features/settings/auth-idle-timeout.lib.ts:10-26` | `6h`=21600000, `12h`=43200000, `24h`=86400000, `3d`=259200000, `7d`=604800000, `never`=disabled |
| Type guard / fallback | same file, `:32-41` | unknown stored values resolve to the caller fallback |
| Default preset | `packages/web/src/features/settings/settings.model.ts:75,94` | `"3d"` |
| Load sanitization | same file, `:175-178` | invalid persisted value loads as `"3d"` |
| Per-owner persistence | same file, `:289-293` | the chosen preset is stored in the current owner's settings |
| Guard lifecycle | `packages/web/src/widgets/layout/layout-auth-guard.hook.ts:14-30` | guard starts only with a session and ready/degraded user status; `never` starts no guard; a preset change tears the old guard down exactly once then re-initializes; expiry navigates to the login route |
| Guard contract test | `packages/web/src/widgets/layout/layout-auth-guard.hook.test.ts:19-109` | default timeout 259200000; `24h` -> 86400000; switch to `never` is cleanup-only (no re-init) |
| Deadline engine | `packages/web/src/shared/lib/auth-guard.ts:112,171-263` | internal 24h default only when the caller omits one (`:112`); idempotent init with a cleanup closure (`:229-263`); activity events `mousemove/keydown/click/scroll/touchstart` (`:239`); each activity resets `deadline = now + timeoutMs` (`:194-201`); the expiry timer is cleared while the document is hidden and rescheduled from the stored deadline when visible again (`:171-214`); on expiry credentials are wiped first and the expiry callback runs even if a hook fails (`:132-169`); read-only `getSessionExpiresAtMs`/`getSessionRemainingMs` (`:217-227`) |

There is no countdown or warning UI in the reference: expiry is a silent
sign-out at the deadline.

## Native module

`packages/native/src/native-auth-guard.{h,cpp}` adds `workspace::NativeAuthGuard`,
a `QObject` that owns one deadline and one `QTimer`.

### Timer model

- The deadline is an absolute millisecond stamp from an injectable clock
  (`std::function<qint64()>`, default `QDateTime::currentMSecsSinceEpoch()`).
- `setTimeoutPreset(preset)` maps the preset through the reference table. A
  valid preset with an active session arms the guard: `deadline = now + timeoutMs`
  and the timer is scheduled for the remaining time. `never` (or an unknown
  preset) tears any existing deadline down exactly once and starts nothing.
- A single-shot `QTimer` is scheduled for `min(remaining, pollIntervalMs)` and
  reschedules itself until the deadline passes, because the injected test clock
  advances independently of wall-clock timer intervals. The production poll
  interval is 1000 ms; the idle thresholds are hours, so this cannot shift an
  expiry by a meaningful amount. Tests use a short interval and a fake clock.
- Any activity event resets `deadline = now + timeoutMs` (the reference's
  `resetTimer` at `auth-guard.ts:194-201`).
- On expiry the guard stops itself first (so the action can never re-enter an
  armed guard), then invokes the injected expiry action exactly once. A throwing
  action is swallowed, mirroring "failures logged, expiry still fires" at
  `auth-guard.ts:132-169`. The guard stays inactive until the next login or
  preset change re-arms it.
- Read-only observers `expiresAtMs()` and `remainingMs()` mirror
  `auth-guard.ts:217-227`; both report the inactive state as `0`/negative.

### Activity sources

While armed, the guard installs an application-wide event filter (the reference's
window listeners) and treats `QEvent::MouseMove`, `QEvent::KeyPress`,
`QEvent::MouseButtonPress` and `QEvent::Wheel` as activity - the Qt counterparts
of `mousemove/keydown/click` plus wheel scrolling. The filter never consumes
events. Window activation changes are not activity: the reference has no
focus-based reset, and treating exposure events as activity would let a window
restore re-arm the deadline it just missed while hidden.

### Hidden-window pause

`auth-guard.ts:171-214` clears the expiry timer while `document.hidden` and
reschedules from the stored deadline when the page becomes visible again, so
background time does not consume the idle budget. The native analogue is
"no top-level window is visible/exposed": the guard recomputes that from
`QGuiApplication::topLevelWindows()` on window visibility/exposure/activation
events and whenever an explicit `setWindowsVisible(bool)` is pushed (tests and
embedders drive it directly). While paused the deadline stamp is retained; on
return, if the stored deadline already passed the expiry fires immediately,
exactly like the reference's `scheduleSessionExpiryTimer`.

Deviation: a minimized window reports `isExposed() == false` on X11, so it
pauses the deadline the same way a hidden window does. The web reference pauses
only on tab visibility; native has no tab equivalent, and the issue prescribes
"no window visible/exposed" as the pause condition.

### Live preset reaction

`NativePreferences` grows a per-owner `authIdleTimeout` preference
(`settings/owners/<owner-hash>/authIdleTimeout`, anonymous scope when no owner
is bound) with the reference sanitization: absent or invalid values resolve to
`"3d"` (`settings.model.ts:75,175-178`), stored values are read back per owner,
and `authIdleTimeoutChanged` fires whenever the effective value changes
(including owner switches). The controller connects that signal to
`NativeAuthGuard::setTimeoutPreset`, so a settings change re-arms the live
deadline immediately - the counterpart of the layout hook's effect dependency
(`layout-auth-guard.hook.ts:14-30`, contract test `:87-109`: switch to `never`
is cleanup-only).

### Wiring and credential wipe

`NativeController` owns the guard:

- `Session::authenticated` -> `setSessionActive(true)` (guard inactive before
  login, mirroring the reference's "guard starts only when a session exists").
- `Session::signedOut` -> `setSessionActive(false)` (guard inactive after manual
  logout).
- `setPreferences()` binds the preset and its change signal.
- The expiry action calls the existing `NativeController::logout()`, so the
  wipe is byte-identical to a manual logout: immediate auth revocation,
  cache cleanup request and `Session::logout()` removing the saved refresh
  credential from the secure store (`packages/native/src/session.h:49`,
  `src/session.cpp:169-188`). No second wipe path exists.
- `authGuardActive` is exposed as a controller `Q_PROPERTY` with correct
  `NOTIFY` emission on every transition.

Interaction with the token expiry already in `Session`: the access-token TTL
timer (`session.cpp:49-51, 284-298`) governs HTTP credential freshness and is
untouched. The idle guard is client-side inactivity only; neither timer reads
the other's state. If the token refresh fails first, the existing session-fail
path resets the controller and `signedOut` disarms the guard like any logout.

## Test plan (CTest group `native-native-auth-guard`)

| Acceptance | Test |
| --- | --- |
| (1) preset conversion, `never` disables | `presetConversionMatchesTheReferenceTable`, `neverPresetKeepsTheGuardDisabled` |
| (2) exactly-once expiry at the deadline | `idleDeadlineSignsOutExactlyOnceAtTheDeadline` |
| (3) activity extends by the full timeout | `activityBeforeTheDeadlineExtendsItByTheFullTimeout` |
| (4) preset change re-arms; `never` cancels only | `presetChangeRearmsTheLiveDeadlineImmediately`, `switchingToNeverCancelsWithoutRefiring` |
| (5) default `3d`, invalid stored -> `3d` | `authIdleTimeoutDefaultsToThreeDays`, `invalidStoredAuthIdleTimeoutSanitizesToThreeDays`, `authIdleTimeoutSetterSanitizesPersistsAndNotifies` |
| (6) inactive before login / after logout | controller group: `authIdleGuardFollowsTheLiveSession` |
| (7) preset survives restart per owner | `authIdleTimeoutSurvivesRestartPerOwner` |
| hidden-window pause | `hiddenWindowsPauseTheDeadlineAndVisibleReturnResumesIt` |
| event-filter sources | `onlyActivityEventsReArmThroughTheEventFilter` |

The controller group adds: `authIdleGuardExpirySignsOutExactlyOnceLikeManualLogout`
(expiry fires `Session::signedOut` exactly once with the same cleared state as
a manual logout) and `authIdleGuardReactsToPreferenceChangesLive` (a preference
change re-arms the live deadline without a restart).

Out of scope: the settings ROW UI (issue #135), any countdown/warning surface
(none in the reference), Windows/macOS acceptance, and live-service behavior.
