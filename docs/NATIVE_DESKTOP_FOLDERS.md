# Folders CRUD: create/rename modals, delete confirm, context menu

Issue #84 (epic #82). Builds on the API-backed folder rail of
`docs/NATIVE_DESKTOP_FOLDERS_RAIL.md` (issue #83): the `folders` projection on
`NativeController` (`m_folders`, `foldersChanged`, sorted folder list from
`GET /folders/`) stays the single rail model; this issue adds the mutation
flows on top of it.

## Reference contract (measured)

Oracle: `workspace_ui` at sha
`61dacef78dab2f5d0be76f283bebf49fe29b84af` (`~/dev/workspace_ui`), deepened by
the backlog-scout matrix read at `230e02b8b0273dee556a406746d122180a68b3e7`.
Design fidelity case: parity-preserving port of an existing React surface with
no new chrome tokens, so the reference code is the pixel oracle and Figma
mockups were not consulted (same treatment as the #83 rail doc).

### Transport (`shared/api/messenger-folders.api.ts`)

- Create: `POST /api/workspace/v1/messenger/folders/` with
  `{title, background_color_value}` (`createMessengerFolder`), response is the
  created folder DTO; the rail then emits
  `onFoldersChanged(created={id,title,backgroundColor})`.
- Rename: `PUT /api/workspace/v1/messenger/folders/<uuid>` with the same body
  shape (`updateMessengerFolder` uses `messengerPutJson`), then
  `onFoldersChanged()`.
- Delete: `DELETE /api/workspace/v1/messenger/folders/<uuid>`, then
  `onFoldersChanged(deletedFolderId)`.
- `background_color_value` is the full ARGB integer (`0xffRRGGBB`), sent as a
  positive number; display code masks to `0xRRGGBB`
  (`folder-colors.lib.ts::folderColorValueToCssHex`).

### Modal internals (`shared/ui/folder-form-modal/folder-form-modal.ui.tsx`)

- One component for both modes (`create` | `edit`); the
  `features/manage-folders` wrappers are thin. Submit resolves
  `Promise<boolean>` and the modal closes only on `true`.
- Title `folder.createFolder` / `folder.renameFolder`; submit label
  `common.create` / `common.save`.
- Name input: autofocus on open (timeout 0), placeholder `folder.folderName`,
  `mb-4 w-full rounded-lg border border-border-subtle bg-bg px-3 py-2 text-sm`,
  disabled while submitting, Enter submits. Submit is blocked while the name
  is empty/whitespace or, in edit mode, when name+color equal the initial
  values (footer submit disabled in both cases).
- Color picker: 8 round swatches `h-9 w-9 rounded-full`, `gap-2.5` (10 px),
  label `folder.color` (`text-xs font-medium text-text-muted`), `aria-pressed`.
  Selected swatch: `border-text-primary ring-2 ring-accent-soft`; others
  `border-border-subtle`. Presets (ARGB): `ff2563eb, ff4f46e5, ff0d9488,
  ff059669, fff59e0b, ffdc2626, ff6b7280, ff334155`; default is the first.
- Preview chip: rounded-lg bordered chip, background = selected color at 12 %
  alpha (`folderColorValueToCssRgba(color, 0.12)`), folder icon 16 in the full
  hex color + `folder.preview` or the trimmed name (`text-xs text-text-muted`).
- Reset: create mode resets name to empty and color to preset 0 on close/open;
  edit mode prefills `initialName` / `initialBackgroundColor` (fallback
  preset 0) on each open; `isSubmitting` resets on open/close.
- Delete confirm (`AppDialog` in `folder-rail.ui.tsx`): title
  `folder.deleteConfirmTitle` "Delete folder?", description
  `folder.deleteConfirmText` `Are you sure you want to delete "{label}"?`,
  cancel + danger delete button with a spinner while pending.
- Context menu (`folder-rail-context-menu.lib.ts`): Rename (disabled for
  system folders), layout toggle, system-folders visibility toggle, Delete
  (disabled for system folders, danger).
- Error toasts: `folder.createFailed` "Could not create folder",
  `folder.updateFailed` "Could not update folder",
  `folder.deleteFailed` "Could not delete folder".

## Native design

### Controller (`NativeController`)

- New single-flight mutation projection `folderMutation`
  (`QVariantMap {kind, pending, error, ticket}`), guarded by
  owner (`creationOwnerKey`) + `m_api->generation()` + ticket exactly like the
  ownStatus update path; the state is cleared on the session/owner reset that
  already drops `m_folders`.
- `Q_INVOKABLE createFolder(name, backgroundColor)` /
  `renameFolder(folderUuid, name, backgroundColor)` /
  `deleteFolder(folderUuid)` issue the three contract requests through
  `m_api->request` directly - deliberately bypassing the tracked request
  helper, because the folder family must never move the connection banner
  (same reasoning as `loadFolders`, see FOLDERS_RAIL "Deviations").
- A settled mutation updates the rail **incrementally**: create appends (or
  replaces, same UUID) a `created` row from the response DTO (color masked to
  `0xRRGGBB` like the loader), rename updates title + color in place keeping
  `streamIds` and selection, delete removes the row and lets
  `selectedFolder()` fall back to All. No `GET /folders/` refetch happens on
  any mutation path.
- Gating: rename/delete resolve the row by UUID or rail key and reject
  anything that is not a server `created` row (system slots, synthesized
  slots, unknown ids) with the reference toast default, without sending a
  request. This mirrors the context-menu disabled state so a stale menu can
  never edit a system folder.
- The rail button keeps emitting `folderAddRequested()`; the QML listens and
  opens the create dialog.

### QML (`Main.qml`, next to the folderRail)

- `folderFormDialog` (Popup, ownStatusDialog chrome): mode-aware title
  (Create folder / Rename folder), `NativeField` name input
  (`folderNameField`, placeholder Folder name, Enter submits), 8 round 36 px
  swatches (`folderSwatch-<i>`, 10 px gap, selected ring in `accent-soft`),
  preview chip (`folderPreviewChip` at 12 % alpha + `folderPreviewText`),
  Cancel + Create/Save (`folderFormCancel` / `folderFormSubmit`) with a
  `StatusSpinner` while pending. Submit is disabled while the name is empty,
  while pending, or in edit mode with no changes; the modal closes only when
  the mutation settles without error (the reference promise-true contract),
  otherwise the error toast opens and the modal stays open. On open, create
  mode resets name + preset 0, edit mode prefills name and the matching
  preset color, and the name field takes focus via a `callLater` (the
  reference timeout-0 autofocus).
- `folderDeleteDialog`: title "Delete folder?", description
  `Are you sure you want to delete "<title>"?`, Cancel + danger Delete with
  spinner while pending (`folderDeleteConfirm`).
- `folderErrorToast`: same surface as `ownStatusErrorToast` (top-right notice
  card, 5 s auto-dismiss, close button).
- `folderContextMenu` (`folderMenuRename`, `folderMenuDelete`): opened from a
  right click on the rail cell (`folder-<key>` MouseArea now accepts the
  right button); both actions are disabled for system folders.
- objectNames: `folderFormDialog`, `folderNameField`, `folderSwatch-<i>`,
  `folderPreviewChip`, `folderPreviewText`, `folderFormCancel`,
  `folderFormSubmit`, `folderDeleteDialog`, `folderDeleteDescription`,
  `folderDeleteConfirm`, `folderContextMenu`, `folderMenuRename`,
  `folderMenuDelete`, `folderErrorToast`, `folderErrorText`.

## States covered

- Create success (row appended without refetch), create failure (toast, modal
  stays open), empty/whitespace name (submit blocked), Enter submit, reopen
  reset, edit prefill + no-change save gating, rename success in place,
  delete confirm text + removal + All fallback, system-folder gating in menu
  and controller, single-flight (second mutation ignored while one is in
  flight), light + dark, en + ru (all new strings carry `ru` translations).

## Test acceptance

- Controller CTest (`native-native-controller`): `folderCreate…` (POST body
  contract, appended row, GET-count stays 1), `folderRename…` (PUT contract,
  in-place row, membership + selection survive), `folderDelete…` (DELETE
  path, row removed, All fallback, sidebar unscoped),
  `folderMutationsGateSystemFoldersAndSurfaceErrors` (no PUT/DELETE on system
  slots, toast texts), `folderMutationsAreSingleFlightUntilSettled`.
- Window CTest (`native-native-window`): `folderRailCreateModalFollowsTheFormContract`
  (focus + preset 0 on open, submit gating, swatch 5 repaints the chip,
  Enter submits, incremental rail append, reopen reset),
  `folderRailContextMenuRenamesAndDeletesCustomFolders` (edit prefill +
  no-change gating, PUT body, rail relabel, confirm text, DELETE, row gone),
  `folderRailContextMenuKeepsSystemFoldersReadOnly` (disabled rename/delete
  on All/Personal, enabled on a created folder).
- Locale guard: `folder.createFailed/updateFailed/deleteFailed` texts plus
  `Create folder`, `Rename folder`, `Folder name`, `Color`, `Preview`,
  `Delete folder?`, `Are you sure you want to delete "%1"?` all have `ru`
  entries in `packages/native/translations/workspace_ru.ts`.

## Deviations and follow-ups

- The context menu ports only Rename and Delete. The reference layout toggle
  and system-folders visibility toggle have no native counterpart yet (no
  rail-layout or visibility settings surface exists), so those items are
  deferred with the overflow quick list from #83.
- The edit modal prefill matches the folder's color against the 8 presets; a
  color outside the presets is carried through as `0xffRRGGBB` (no swatch
  shows selected) so a rename never repaints it silently. The reference keeps
  the exact store value the same way (fallback preset 0 only when unset).
- Folder-name sanity bound in the controller is 200 chars (the modal caps
  input at 200 too); the reference modal has no explicit bound.
- Realtime folder mutation events still do not touch the rail (unchanged from
  #83); the mutation responses are applied locally as described above.
