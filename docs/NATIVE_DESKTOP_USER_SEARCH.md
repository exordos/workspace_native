# Global user search

The reference search modal is in `widgets/search-modal` at workspace_ui
61dacef78dab2f5d0be76f283bebf49fe29b84af. The primary Workspace session
on Orion rendered the top-bar and conversation Search buttons with the
HTML hidden attribute. Neither exposed an accessible action. The reference
top-bar handler also returns false when a person has no direct conversation.
Issue #138 calls for an accessible entry and creation of that conversation.

The top-bar Search button opens a focused dialog near the upper center of the
window. An empty query shows no entries. A trimmed, case-insensitive substring
matches display name, username, email, status text or status emoji in the
current Workspace user directory. Show at most 20 people. Each row shows
presence, the name and the resolved status label or email. An emoji-only status
remains a visible label. A nonempty query without matches shows No results
found. Closing the modal abandons its query.

Selecting a person uses the native direct-chat navigation and creation flow.
An existing direct conversation opens immediately. A newly created conversation
uses the normal pair identity, canonical stream and default topic confirmation.
Keep a failed selection available for another attempt. Local message history
search is tracked independently in #10.
