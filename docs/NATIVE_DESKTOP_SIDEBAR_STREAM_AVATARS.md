# Native sidebar stream-row avatar background color (issue #176)

Reference oracle: workspace_ui at sha `61dacef78dab2f5d0be76f283bebf49fe29b84af`.
The Figma token was available, but every geometry and color fact below was
measured directly from the cited reference sources at that sha, so the
reference code is the design oracle.

## Contract (reference, measured at 61dacef)

`packages/web/src/widgets/sidebar/sidebar-workspace.ui.tsx`:

- Line 417: `const avatarSize = compact ? "sm" : "md";`
- Lines 421-428: an encrypted stream renders its avatar content as
  `<Icon name="chatLock" size={24} className="text-text-primary" />`
  inside the same circular Avatar (glyph over the stream-colored circle).
- Line 503-510: `directPrivate` streams render `WorkspaceAvatar`
  (photo/letter/presence) - never a stream background color.
- Lines 512-519: every other stream renders
  `<Avatar size={avatarSize} style={{ backgroundColor:
  resolveAvatarBackgroundColor(stream.color) }}>` with the initial letter as
  children.

`packages/web/src/shared/lib/avatar-color.lib.ts:2-7`
(note: at this sha the helper lives in `shared/lib`, not
`features/workspace-avatar`): `resolveAvatarBackgroundColor(color)` maps a
validated API RGB integer `0..0xffffff` to `#rrggbb` (`0` renders `#000000`);
missing, non-integer or out-of-range values return `undefined` and the Avatar
keeps its theme fallback background.

`packages/web/src/shared/ui/avatar/avatar.tsx:6-11,61`: `sm` is `w-8 h-8`
(32 px), `md` is `w-10 h-10` (40 px); base classes are `rounded-full bg-bg
border border-border-subtle font-semibold text-text-primary`, so the fallback
circle is the `bg` token and the initial letter uses `text-primary`.

`packages/web/src/app/app.styles.css:298,303` (Blue Cold light fallback for
`:root`): `--color-bg: #e4ecf3` (the `bg-bg` token), `--color-text-primary:
#1b1b1d`. The dark-mode values are applied through the theme engine per
`[data-theme=dark]`: `packages/web/src/shared/lib/themes/blue-cold.ts:8,71`
gives `bg: "#141517"` (dark) and `bg: "#e4ecf3"` (light). The initial letter
color and the fallback token therefore both follow the active theme.

## Native state today

- `qml/Main.qml` (sidebar stream delegate, `objectName: "stream-" + id`,
  avatar `Rectangle` at ~line 3316): the circle is already `40x40` radius 20
  with `border.color: border-subtle`, an encrypted row already shows the 24 px
  `chat_lock.svg` glyph in `text-primary`, and the initial letter already uses
  `text-primary`. The only gap: `color: root.colors.bg` is hardcoded, so the
  stream `color` from the API is ignored on every non-direct row.
- Direct rows already render photo (`NativeAvatarImage` from
  `modelData.avatar`) or letter on `root.colors.bg` - this must stay true even
  if a direct stream DTO carries a `color`.
- The identical derivation already exists in two native surfaces:
  `src/native-controller.cpp:6180` (drafts row
  `avatarColor = !direct && stream->color ? "#%1".arg(*stream->color, 6, 16,
  '0') : QString{}` consumed by `qml/NativeDraftsPage.qml:181-185`, which
  falls back to `page.colors.bg`) and `src/native-controller.cpp:3873-3884`
  (channel-info header). `src/domain.cpp:444-446` already validates the DTO
  exactly like `resolveAvatarBackgroundColor`: the color is only accepted as
  an integral double in `0..0xffffff` (`0` kept), so an unset/out-of-range DTO
  color leaves `Stream::color` empty and the QML fallback applies.
- The sidebar model `NativeController::streams()`
  (`src/native-controller.cpp:11120-11160`) exposes `id`, `name`, `encrypted`,
  `isDirect`, `avatar`, `presence`, ... but not the stream color.

## Design

Reuse the existing derivation; no new user-visible strings:

1. `src/native-controller.cpp` - in `streams()`, hoist the existing
   `isDirect` expression and add one row field with the drafts derivation:
   `avatarColor = !direct && stream.color ? QString("#%1").arg(*stream.color,
   6, 16, QLatin1Char('0')) : QString{}`. Direct rows and null colors carry an
   empty string, exactly like the drafts row.
2. `qml/Main.qml` - the sidebar avatar `Rectangle` color binding becomes the
   NativeDraftsPage.qml:182-183 pattern:
   `!isDirect && avatarColor.length ? avatarColor : root.colors.bg`.
   The encrypted lock glyph and the direct photo/letter paths are untouched
   and now sit on the stream-colored circle automatically.

Fallback token parity: native `root.colors.bg` is `#e4ecf3` in Blue Cold
light and `#141517` in dark (`qml/NativeTheme.qml:107`, mirrors
`app.styles.css:298` and `blue-cold.ts:8,71`), and `text-primary` is
`#1b1b1d`/`#ffffff` - so null colors and letter colors already track the
theme exactly like the reference.

## Measured geometry

| Element | Reference (61dacef) | Native (this change) |
| --- | --- | --- |
| Sidebar avatar circle (md / non-compact) | 40 x 40 (`avatar.tsx:9`) | 40 x 40, radius 20 (`Main.qml` stream delegate) |
| Sidebar avatar circle (sm / compact) | 32 x 32 (`avatar.tsx:8`) | native sidebar has no compact mode |
| Encrypted glyph | chatLock 24 px, `text-text-primary` (`sidebar-workspace.ui.tsx:424`) | `chat_lock.svg` 24 px, `text-primary` (already) |
| Initial letter | `text-xs` 12 px semibold, `text-text-primary` | 16 px Medium, `text-primary` (pre-existing, out of scope) |
| Fallback circle | `bg` token `#e4ecf3` light / `#141517` dark | `root.colors.bg`, same values |

Because the initial letter and the lock glyph are centered in the circle, a
window test cannot read the circle color from the geometric center pixel (the
glyph covers it); the probes sample the annulus between the glyph box and the
circle edge (inset ~4-6 px from the top/bottom/left/right of the 40 px box),
which is inside the circle and outside the glyph - pixel-equivalent to the
reference rendered result.

## Acceptance (window group `native-native-window`, tests/native-window-test.cpp)

Scene: a mock catalog serving five sidebar rows - a channel with color
`0x2563eb`, a channel with color `0`, a channel without a color, an encrypted
channel with a color, and a direct row that carries a color which must be
ignored. Assert via `avatarProbeCircle` + `window->grabWindow()` pixel probes:

1. `0x2563eb` channel row circle samples `#2563eb`.
2. `0` channel row circle samples `#000000`.
3. colorless channel row circle samples the theme `bg` token (`#e4ecf3` light).
4. direct row circle stays the `bg` fallback (photo/letter unchanged) even
   though the DTO carries a color; letter/presence behavior is untouched.
5. encrypted channel row shows the visible `chat_lock.svg` glyph over a
   stream-colored circle (annulus sample equals the stream color).
