# Search cached messages

The signed-in desktop opens a dedicated message search dialog from the top bar. The dialog identifies its scope as the selected account and project on this device. The reference search modal supplies the keyboard layout. The product decision for native message history is recorded in issue #10. In the deployed web client the two search buttons in the conversation were hidden and Ctrl+K did not open a dialog.

A nonempty search matches readable message text case-insensitively, including Russian, and treats punctuation literally. The client searches cached message bodies and history pages for the current server, project and user on the cache worker. Duplicate copies of a message produce one result using the latest saved version. The dialog displays up to 50 matches; narrowing the query searches the full cache again. Markdown contributes visible text, polls contribute their questions and options. Encrypted wire payloads and key events have no searchable local plaintext. Search creates no persistent copy of a message.

The results show an excerpt, its conversation and date. The dialog distinguishes an empty query, a search in progress, no matches and an unavailable cache. Up and Down select a result, Enter uses the saved stream and topic to open the matching row through the normal history and anchor checks, and Escape returns focus to the launcher. Closing the dialog or switching accounts clears the query and results.

[Native scope decision](https://github.com/exordos/workspace_native/issues/10#issuecomment-7041378950) · [Reference dialog](https://github.com/exordos/workspace_ui/blob/230e02b8b0273dee556a406746d122180a68b3e7/packages/web/src/widgets/search-modal/search-modal.ui.tsx)
