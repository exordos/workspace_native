# Settings and Appearance in the account drawer

The active Workspace profile opens Settings and Appearance as nested screens.
The reference is workspace_ui at 61dacef78dab2f5d0be76f283bebf49fe29b84af,
in right-panel-user-settings.ui.tsx and right-panel-option-list.ui.tsx.
I opened both screens under the primary Orion account. Chat click had two
options, notification sound had six, and auto sign-out had six. Appearance
offered theme, stream and topic order, folder layout and chat-list density.
Opening and collapsing the groups preserved the selected values.

Reuse the right drawer's Back and Close controls. Each option group exposes its
expanded state, its selected option and the current value. The settings screen
uses the existing language, chat-click and idle-timeout preferences. The chat
list and idle guard already observe these values. Appearance uses the existing
theme, palette, folder layout and density settings. Keep the public API and
storage of those preferences so existing installations retain their choices.

Notification sound and sidebar ordering need persisted values. An explicit
sound selection plays one short preview, using the reference frequencies,
waveforms and timing from notification-sound.ts. Off is silent. The same
preset feeds message notifications. Ordering updates both visible streams and
their topics. Use active unread state and message activity for Unread first,
and a locale-aware, case-insensitive numeric stream-title comparison for By name.
The reference topic comparator keeps topic groups and their activity order in that mode.
Last message keeps the existing initial order. Check the full flows through
the normal profile control in English and Russian.
