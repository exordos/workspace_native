# Topics in the channel information drawer

The reference is the right-panel topic list and topic summary at workspace_ui
61dacef78dab2f5d0be76f283bebf49fe29b84af. In the signed-in Workspace I
observed 24 topic rows in the selected channel. The list initially expanded,
collapsed on its header and reopened. The selected topic had a pending summary.
Opening the gear showed AI summary settings.

The channel drawer uses its current stream’s topics. Completed topics are
omitted. Keep their existing ordering, effective notification mode and unread
counts. Empty topic names use the localized General chat label in italics.
Display a personal mention independently of its unread count. Each row opens
its topic, and its context menu addresses that row even when another topic is
selected.

The selected topic supplies the Topic context block. The topic API returns
`summary`, `summary_has_new_messages` and `summary_enabled`. The default
is enabled for topic records that predate the fields. A text summary starts
expanded and uses the standard native Markdown renderer. An absent summary
starts collapsed. Expanding it shows the pending or disabled state. Render
the new-messages note only alongside existing text. Reset expansion when the
selected topic changes.

The settings gear follows the current user's channel owner or administrator
role, or the exact IAM permission `workspace.topic_summary_settings.manage`
or `workspace.topic_summary_endpoint.manage`. The server retains authority
over settings changes. Scope any responses and pending state to the active
session and topic. The topic-specific settings and topic context actions
use the Messenger topic endpoints.
