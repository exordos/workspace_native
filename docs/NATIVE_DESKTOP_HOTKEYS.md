# Keyboard shortcuts

The native window uses the Workspace shortcut catalog. Navigation has eight
actions, the interface has five, and the composer has five. Five fixed entries
describe standard editing and navigation keys. A binding can use Mod, which
means Command on macOS and Control on Windows and Linux. Conflict checks
compare physical chords on both platforms, including shifted punctuation.

The application records US QWERTY positions from key events. Repeats,
modifier-only events, composition and AltGr leave normal text entry alone.
A shortcut applies only in its window and scope. Composer formatting requires
the editor to have focus. A visible modal keeps global commands from acting
behind it. Quit, fullscreen, normal copy and paste keep their native handlers.

The settings INI stores `hotkeys/enabled` and `hotkeys/overrides/<id>` in the
active owner's settings. An absent entry restores an action's default. An empty
binding disables that action. The reader normalizes stored overrides until the
remaining bindings are valid and distinct. Saving an invalid binding leaves
the current shortcut in place.

The Settings drawer opens a searchable Keyboard shortcuts dialog with the three
remappable sections and one fixed section. Selecting a shortcut arms its
recording field. Save applies an allowed chord, Escape cancels recording, and
Disable or Reset changes the selected row. The help overlay lists the currently
assigned and fixed shortcuts. Both dialogs use the shared theme and translations.

Reference: workspace_ui hotkeys.lib.ts, hotkeys.tsx, settings.model.ts and the
shortcut settings and help views at 61dacef.
