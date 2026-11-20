# External accounts in the account menu

The reference is right-panel-external-account.integration.tsx and the
connect-external-account feature at workspace_ui
61dacef78dab2f5d0be76f283bebf49fe29b84af. In a fresh primary Orion session,
Profile → External accounts → Connect service opened the Zulip selector. The
close control returned to the list. No provider change was submitted.

Show accounts only for the current server, authenticated user and project.
Expanding the list refreshes all pages of the Messenger external_accounts
collection. The Workspace backend enforces the owner/project filter and
read/create/update/reconnect/delete permissions. Keep peer credentials out of
the list projection, the disk cache, logs and error messages.

Use the ordinary project transport. Create supplies a client-generated UUID,
Zulip settings and a write-only API key. Keep that UUID for a retried submission
of the same form. Binding an imported Zulip account preserves its server UUID.
Read that canonical UUID in the current project and check the submitted server
and email before showing it. Edit reads the current account, preserves its provider options
and sends the replacement settings with the current ETag. Reconnect updates a
credential for a degraded or authentication-required account. Delete requires
confirmation. When a form or the account menu closes, discard the secret and
cancel outstanding work. A later open refreshes the server's canonical state.
