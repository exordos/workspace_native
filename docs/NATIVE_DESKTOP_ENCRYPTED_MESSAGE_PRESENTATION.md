# Preparing encrypted conversations and sidebar previews

The reference conversation shows a busy surface during the first encrypted batch. The implementation follows the Workspace UI revision pinned by issue #228.

## Conversation

A newly opened conversation with pending presentations reserves the whole message viewport and exposes a busy state to accessibility. It shows no placeholder bubbles. When the first batch settles, the list opens. Later batches retain the already presented rows, filter pending UUIDs, and defer paging requests until preparation settles. Sending a message opens the window immediately for the outgoing row. Its confirmation can keep the temporary presentation until the corresponding server row is ready. Switching conversation, account or runtime starts a fresh window. Existing missing-key and failure actions apply to the settled results. The empty surface has the same geometry in both themes and contains no translated text.

The history updates its outgoing row when the pending message identity or text changes. Other controller notifications preserve the viewport and the user’s scroll position.

[Workspace message list, lines 736–794](https://github.com/exordos/workspace_ui/blob/230e02b8b0273dee556a406746d122180a68b3e7/packages/web/src/widgets/workspace-message-list/workspace-message-list.ui.tsx#L736-L794)
