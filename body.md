## Summary

Port the reference Open Source Licenses browser (issue #46) to the native
desktop client as a new section page, pixel-measured from the pinned
reference `packages/web/src/pages/licenses/licenses-page.ui.tsx` at
workspace_ui `61dacef78dab2f5d0be76f283bebf49fe29b84af`. Figma token
unavailable - mockups not consulted; the reference code is the oracle.

- `docs/NATIVE_DESKTOP_LICENSES.md` (first commit): full measured contract
  (geometry, token table from `blue-cold.ts`, i18n keys EN/RU, close-route
  semantics) plus the dataset/tooling design.
- New `qml/NativeLicensesPage.qml`: title/subtitle with count, 36 px rounded
  close button, top-8 license-type chips with toggle-off and Reset chip,
  search matching name OR publisher (case-insensitive substring), the
  "{filtered} of {total} packages" counter, sticky-header table with the
  four measured columns, accent external links only when a repository is
  present, and the empty-filter-result state.
- Dataset: new functionality - the native client's own third-party
  dependencies (Qt 6 modules, md4c, SQLite, Qt6Keychain, OpenSSL, ...),
  emitted deterministically by `scripts/collect-native-licenses.py`
  (release-notes.py precedent) into
  `qrc:/workspace/licenses/licenses.json` at build time and handed to the
  page through `NativeController::licensesDatasetJson()` (QML XHR refuses
  local-file GETs). The React app npm dataset is not used.
- Entry surfaces: About drawer "Open Source Licenses" link
  (`nativeAboutLicensesLink`, signal `licensesRequested()`) beside the
  policy-style rows, plus the whitelisted `/licenses` and
  `/org/:orgId/licenses` deep link now routed in `applyDeepLink` to the new
  `showSection("licenses")` state (`ContentPage::Licenses`). Close resolves
  per the reference tests: section history back when a non-chat section
  preceded the open (`sectionBeforeLicenses`), else the messenger home.
  External links hand off through `Qt.openUrlExternally` (the
  QDesktopServices path already used for markdown links).
- i18n: every new `qsTr` string added to
  `packages/native/translations/workspace_ru.ts` (ru + en verified).
- Electron: n/a.

Closes #46

## Tests (TDD, failing first)

- `native-native-window`: `licensesPageStates` (4 data rows: en/ru x
  light/dark - title/subtitle fonts and colors, top-8 chip set, labels and
  counts, top-8 cap excludes the 9th type, chip toggle + selected colors +
  Reset chip, toggle-off, search by name and by publisher, mixed case, empty
  result with "0 of N packages" and no rows, accent link vs plain-text row
  colors, full-width page), `licensesCloseResolvesBackVersusMessengerHome`
  (back vs fallback per reference tests lines 47/56),
  `licensesExternalLinkHandsOffToTheBrowser` (handoff stubbed via
  `QDesktopServices::setUrlHandler` and asserted; plain-text row never
  hands off). 3 functions / 4 data rows green.
- `native-about-policy`: `licensesEntryLinkEmitsRequestedWithoutTouchingUpdater`
  - entry link visible, accessible name "Open Source Licenses", click emits
  `licensesRequested()` and never touches the updater. Full binary 12/12.
- `native-native-locale`: catalog guard green (14/14) after adding the new
  ru strings.
- Tooling (not registered in CTest; group count unchanged):
  `python3 packages/native/tests/licenses-dataset-test.py` - 6 checks green
  (byte-stable determinism, five-key entry shape, required native deps
  present, sorted order, plain-text-cell presence, top-8 census).
- Live server verification: results appended below (## Live verification).

## Live verification

Verdict: PASS (live staging, final head 9e6136a, binary build/lane/workspace-native,
display :35, evidence dir ~/dev/qa/evidence/46/).

- native-login: full OTP login succeeded (stage4_workspace.png; fresh
  session, no credential output).
- Deep link entry: launching the binary with `ew://open/licenses`
  forwarded to the running client and rendered the page (licenses-deeplink.png):
  title "Open Source Licenses", subtitle "Workspace uses 16 open source
  packages", chips MIT (5) / LGPL-3.0-only (3) / BSD-3-Clause (2) / FTL (1) /
  Apache-2.0 (1) / LGPL-2.1-or-later (1) / blessing (1) / Zlib (1),
  counter "16 of 16 packages", package table with accent links.
- Live chip toggle: clicking the MIT chip filtered to "5 of 16 packages"
  with the chip selected and a Reset pill shown (licenses-filtered.png);
  the close button returned the client to the messenger (after-close.png).
- About drawer entry: account menu -> "App version" drawer shows the
  "Open Source Licenses" card (about-drawer.png); clicking it closed the
  drawer and opened the page (licenses.png, counter "16 of 16 packages").
- health --repeat 10: p50 7.55 ms, p95 8.1 ms (< 500 ms requirement).
- send --stream-uuid 59033cd5-... : send_ms 64.2 (< 1500),
  visible_in_list_ms 31.2 (< 3000), read_back true
  ("CASSI QA 46 2026-11-17T13:52Z", message_uuid
  be1200f9-7d52-5773-a3b2-81c664c49787).

