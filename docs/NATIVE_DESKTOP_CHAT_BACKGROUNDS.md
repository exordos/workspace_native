# Chat backgrounds (native, issue #637)

Parity clone of the reference `packages/web/src/features/chat-background`
(workspace_ui ca73acf7a5e779946d0af72f04bf30a0f84e4089). Local-only feature:
no message, upload, backend or Electron contract changes
(reference docs/CHAT_BACKGROUNDS.md:7,11). Design fidelity: parity-preserving.

## 1. Value model (reference `chat-background.lib.ts`)

```
Value   = {preset:<id>} | {image:<dataURL>} | null (default)
Prefs   = {global: Value, chats: {<chatKey>: Value}}
chatKey = stream:<uuid> | topic:<uuid>          regex ^(stream|topic):[0-9a-f:-]+$ (i)
presets = mountains | sea | forest              (bundled SVG assets, aspect 8/5)
```

Restore/validation rules cloned verbatim:

* whole stored raw string longer than 4 000 000 chars -> empty prefs;
* `chats` entries: key must match the chat-key regex, otherwise dropped; a
  `null` entry is kept as the explicit default; an invalid value drops the key;
* a preset is accepted only when the id is one of the three;
* an image is accepted only as `data:image/jpeg;base64,[A-Za-z0-9+/]+={0,2}`
  with length <= 1 998 990 (`MAX_STORED_LENGTH - len('{"image":""}')`);
* a bare legacy value (no `global`/`chats` wrapper) validates as the global
  value only when the raw length <= 2 000 000;
* saving prefs whose serialized form exceeds 4 000 000 chars is a storage
  error and keeps the previous background.

Resolution: an explicit `chats[chatKey]` (including null) wins over `global`;
absent key falls back to `global`; global null renders the untouched theme
surface. Apply scope `all` replaces `global` and clears every `chats` entry;
apply scope `chat` writes only `chats[chatKey]`.

## 2. Persistence

Reference keys localStorage per runtime owner (`workspace-chat-background-v1:`
+ ownerKey, live cross-tab via events). Native analogue: the value is stored
per runtime owner inside the existing profile preferences file
(`NativePreferences`, QSettings ini), key `chatBackground.v1:<ownerKey>`, as
the reference JSON shape. The owner switch resets the in-memory prefs and
reloads that owner's stored prefs, which reproduces both the keying and the
stale-owner write guard (writes after the owner left are dropped). No sync.

## 3. Image pipeline (reference `prepareChatBackground`)

Input: local raster bytes + declared MIME. Rules: type in
{image/jpeg,image/png,image/webp}; 1 B .. 8 MiB; the file's magic signature
must match the declared type; decoded raster must be valid with
width*height <= 24 000 000; the stored image is JPEG (quality 80) on white
fill, downscaled to fit 1600x1600; the resulting data URL must stay under the
stored-image cap. Errors map to the localized line: type / size / decode /
storage. SVG and external URLs never enter storage.

## 4. UI surface (reference `chat-background.ui.tsx`)

* Settings Appearance section gains the fifth entry "background"
  (icon `images`, trigger objectName `right-panel-appearance-background-trigger`).
* Panel: 2-col grid gap 8; preset card = image aspect 8/5 cover, rounded 8,
  border-border-subtle, caption 12/16 truncate px-2 py-1.5; selected card
  border-accent + ring-2 ring-accent + check badge size 14 at 6px top-right on
  bg-accent; add tile plus icon 28 with processing caption; custom preview row
  40x64 rounded; "Default background" ghost reset; localized error line,
  role=alert.
* Confirmation AppDialog max-w-md: preview max-h-48 rounded-lg cover, "open a
  chat" hint off-conversation, footer `grid-cols-1 sm:grid-cols-[auto_1fr_1fr]`
  gap 8 (Cancel ghost / "Only this chat" outline disabled outside a
  conversation / "For all chats" primary); focus returns to the trigger.
* All controls disabled while unauthenticated (reference `available`).
* The message-list surface (chat page) paints the resolved background: size
  cover, position center; default renders the unchanged theme surface.
* i18n: `chatBackground.*` keys cloned into both catalogs (ru + en).

## 5. Test seams

* Controller group `native-controller-test`: parse/validate/apply/resolve
  contract incl. caps and the regex; per-owner isolation; storage-error keeps
  the previous value; restart persistence (same prefs file re-read).
* Window group `native-native-window`: settings row -> preset selected shows
  the check badge and repaints the message list; per-chat override beats
  global; unauthenticated disables the section.
* Image pipeline unit rows in `native-controller-test`: magic/type/size/
  dimension rejections and the downscale-to-1600 JPEG result.

## 6. Increment order

1. Storage core `chat-background.{h,cpp}` + validation + controller wiring
   (this increment).
2. Image pipeline `prepareChatBackground` on the decode worker.
3. QML Appearance section + apply dialog + message-list surface + i18n.
4. Window-layer contract cases + live verification.
