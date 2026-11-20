# Queued conversation links from Inbox

Issue #335: a link queued before authentication must dispatch once startup has
settled, including when there is no saved conversation or an unavailable saved
conversation falls back to Inbox. The existing selected-stream prerequisite
blocks these cases indefinitely despite a verified, accessible catalog.

Keep the existing queued URL, settling timer, route parser and organization /
project authorization. Use catalog verification and restoration completion as
the readiness boundary; an empty selected stream is a valid Inbox state. Do not
navigate during provisional cache publication, a blocked sync, or an active
saved-topic lookup. The canonical route handler still validates the destination.
No extra retry loop, queue, persistence or network API is needed.

Controller regressions reproduce both unavailable and absent saved-conversation
cases before the change, with an existing saved stream as a passing control.
The visible Qt Quick tests must also assert the destination message body after
startup in English/Russian and light/dark. Keep existing running-app links and
quiet Inbox fallback checks. Synthetic tests are separate from live Workspace
and operating-system URL registration acceptance.

On macOS the installed preview declares the `ew` URL scheme. Launch Services delivers URLs to Qt as file-open events. The application passes validated `ew://` URLs to the same activation and routing code used by a second launch. When the controller is still starting, it retains the most recent validated URL. The package check opens an `ew://` link with the installed application selected and verifies that its original minimized window reappears.
