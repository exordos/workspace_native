# One-time-code login challenge

Issue #59 adds the separate one-time-code step to the native sign-in wizard.
The pinned web reference treats a 401 from the initial IAM token request as a
challenge, retries the same credentials with `X-OTP`, and keeps the code form
open after a rejected code. Only token-endpoint 401 is a code challenge; other
transport and project-discovery failures remain errors. The initial request has
no `X-OTP` header. The pinned OTP form and step transitions also match the
current `workspace_ui` default branch inspected during this work.

The challenge is scoped to the active sign-in attempt. Credentials needed to
retry are held only in memory and never persisted. The credential form remains
hidden while the code form is shown, so Back restores both fields as in the web
client; its password field is destroyed on a new organization, success, or sign-out.
Session's separate challenge copy is discarded on Back, a new login, sign-out,
or a transport identity change. A transient error on the code request retains
the challenge and code for retry and shows the reference's localized login error.
If project discovery fails after an accepted code, temporary tokens are cleared
and the same owned challenge remains available for an explicit retry.
The code form accepts at most six ASCII digits, enables confirmation only for a
complete code, clears a rejected code, and offers Back to credentials. EN and RU
copy follows the pinned reference. On the deployed 1440 × 900 Linux login form,
the code field measures 47 px and both actions measure 36 px. Its centered
six-digit hint stays visible when the empty field receives focus. Back follows
Confirm with a 16 px gap.
The test transport uses synthetic credentials.

Session tests exercise actual loopback HTTP for a 401, rejected code and success,
including headers and stale-attempt behavior. Visible Qt window tests cover form
mounting, keyboard filtering, error, retry, navigation and localized copy. These
local checks do not establish live IAM, platform, or full visual acceptance.
