# Poll text in replies and forwards

When a user quotes or forwards a poll, the text preserves the order and spelling
of its questions and options. Each question starts a block. Each option starts a
new line with a bullet and a space. One blank line separates question blocks.
The text is plain in both languages and both themes, including any Markdown-like
punctuation the poll author typed.

The shared snapshot builder uses the poll payload and the current user's poll
state. An option gets a decimal count in parentheses when results are always
visible, the user voted on that question, the poll is closed, or its closing
time has arrived. A missing option count is zero when a question state exists. Without that state,
the options carry no counts. A vote on another
question does not reveal this question's counts.

The conversation quote frame resolves both loaded and fetched sources. Selected
text takes precedence over the whole poll. The composer uses the same summary
for its reply preview and embeds that full snapshot in the sent quote. A forward stores the summary in the reference with its
plain snapshot format so it continues to show what the sender could see when
they forwarded the poll.

The web reference is `messenger-poll-text.lib.ts`, the message quote frame,
the chat-page reply handler, and the forward-message builder in workspace_ui
at `230e02b8b0273dee556a406746d122180a68b3e7`. Controller tests exercise count
visibility, selected text, lookup and the serialized forward. Window checks
cover the visible quote and composer text under both themes and locales.
