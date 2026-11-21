# Encrypted spoilers in conversation messages

A decrypted Workspace message can contain an inline link or image whose target
is a version-one secret URN. The URN identifies a stream, a key generation and
an encrypted envelope. Ordinary Markdown spoilers use a different syntax.
The web reference is the encrypted-secret-fragment presentation and its hook
in workspace_ui at 61dacef.

The conversation keeps the original encrypted target for forwarding and replies.
The mounted bubble shows a lock for each fragment. A missing key exposes the
localized request action when the referenced stream is accessible. The existing
key-request workflow verifies current authority and owns duplicate prevention.
With a usable key the lock toggles the fragment in place. Failed authentication
offers retry. Closing the disclosure, leaving the conversation, changing accounts
or removing the key clears the displayed text. Secret-bearing bodies suppress
selection. Summaries, notifications and nested secrets use the closed placeholder. The desktop notification transport receives that placeholder for protected Markdown links and images, including percent-encoded targets. Ordinary text and Markdown keep their notification formatting.

The native reference parser verifies the URN and the existing chat-crypto
provider authenticates the envelope using its stream and key generation.
The view identifies a fragment by its position in the canonical current message.
It never supplies replacement ciphertext or a choose-your-own key address.
Decrypted fragment text belongs only to the mounted presentation. The shared
Markdown cache, clipboard, message cache and notification transport retain the
canonical body or placeholder.

The acceptance fixture uses generated group keys and a real HTTPS test service.
The UI test starts with a locked fragment, requests its key, observes the
pending action, installs the key, toggles the plaintext and removes the key
while the message is still on screen. Both locales and themes use the same path.
