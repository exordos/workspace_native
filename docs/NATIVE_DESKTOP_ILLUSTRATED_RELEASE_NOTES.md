# Illustrated release notes for Workspace Native

Status: draft (issue #662). Owns: release-notes structure, illustration assets,
capture workflow, publishing contract. Reference for structure and presentation:
the VS Code release notes page (short highlights overview, then per-feature
sections, each showing what changed and where to find it, with screenshots and
short animations where motion communicates better than prose).

## Problem

`docs/NATIVE_DESKTOP_RELEASE_NOTES.md` is the release notes of record: every
version's own section newest-first, separated by `---`. The tag job extracts the
released tag's section with `packages/native/scripts/release-notes.py` and
publishes it as the GitHub release body. Today the sections are bullet prose
only, so a user skimming a release cannot see what a feature looks like or where
it lives in the client.

## Notes structure contract

The extractor is the contract boundary and does not change: a section starts at
`# Workspace Native X.Y.Z ...` and ends at the first `---` line. Everything
below is added INSIDE that envelope, so extraction per tag keeps working.

Each new version section is ordered:

1. `# Workspace Native X.Y.Z - Linux development preview` (unchanged heading).
2. `In this release` bullets - the existing 3-6 highlight bullets, kept first so
   a text-only reader (RSS, `gh release view`) still gets the summary.
3. Zero or more illustrated feature blocks, one per user-facing feature that
   ships or changes visibly in this release (typically 2-5):

   ```markdown
   ## Chat backgrounds

   ![Chat background picker over a topic](https://raw.githubusercontent.com/exordos/workspace_native/master/docs/images/releases/0.45.0/chat-backgrounds.png)

   What changed: pick a preset or your own image as the background of one chat.
   Where: chat header > Background in the topic menu.
   How it works: the image is validated and resized on save; oversized files are
   rejected with a hint and the previous background stays active.
   ```

   - heading level 2 (never level 1; level 1 belongs to the version heading);
   - exactly one illustration per block (the image line stands alone in its
     paragraph: the neighbouring lines are blank or block-level boundaries
     such as headings), with a descriptive alt text;
   - the three probes `What changed / Where / How it works` as short prose;
     `Where` names the real menu/button path in the current UI;
   - a block may use a short animation (`.gif`) instead of a still when the
     interaction is motion (popups, drag, progress), capped per the asset
     budget below.
4. The `Fixed:` bullets (current two lines), unchanged, after the feature blocks.
5. The platform/target sentence and the `Full changelog from ...` link,
   unchanged and last (release tooling and habit depend on the compare link
   being in the section).

Releases with no user-visible feature (fix-only) keep the current prose form and
may carry zero illustrations.

## Illustration asset contract

- Committed to the repository under
  `docs/images/releases/<version>/<slug>.png` (`<slug>.gif` for animations),
  lowercase-hyphen slugs. Precedent for committed doc images:
  `docs/design/222-figma-mockup.png`.
- Referenced by ABSOLUTE raw URL
  `https://raw.githubusercontent.com/exordos/workspace_native/master/docs/images/releases/...`
  in both the aggregate doc and the extracted body, so the image resolves from
  the GitHub release page, the rendered markdown on any ref, and plain text
  views (alt text). Relative paths are forbidden: the release body is not served
  from the repo tree.
- The repository is PUBLIC. Illustrations must never contain real user data,
  real workspace names, real e-mail addresses or message content from a real
  account. Captures use the synthetic QA scene (see capture workflow) or
  clearly fictional fixture text.
- Budget: max 6 illustrations per release; each still <= 500 KB, each GIF
  <= 5 MB, image source width <= 1440 px (the standard window geometry),
  Retina 2x exports are fine as long as the budget holds.
- Version sections are append-only history: never restyle or illustrate older
  versions retroactively; link rot fixes only.

## Capture workflow

Illustrations are captured from the real client on the QA display, driven by
the existing harness (`~/dev/qa/live_qa.py`, lane-side tooling):

- `live_qa.py run --binary <path> --display :NN --out <png>` smoke-launches and
  shoots; `live_qa.py shot --display :NN --out <png> [win]` shoots the root or
  one window by name fragment while a scene is up.
- A release bump PR that adds illustrations lists the capture scene per image in
  the commit message (fixture set + steps), so a later re-measure can re-shoot
  the same scene; captures show the client window only, no desktop chrome.
- Images are checked on review at the PR stage for the privacy rule and the
  budget; oversized or real-data images are rejected there, not at release time.

## Publishing

No pipeline change: the tag job keeps extracting the tag's own section and
passing it to `gh release create --notes-file`; image lines pass through the
extractor untouched. Guard: `packages/native/tests/release-notes-test.py` gains
a case proving a section containing an image paragraph extracts intact (and a
`## ` subheading does not split the section); the extractor itself needs no
change. The same file additionally audits every image in the aggregate document
against this contract - standalone image line, descriptive alt text, absolute
raw URL in the section's own version folder, committed asset, and the count and
size budgets - so a non-conforming illustration fails the CI tooling check
instead of only a human review. This test file is tooling-side (not registered
in CTest) per the release tooling rule.

## Acceptance

- A release section published for a feature release contains at least one
  `![...](https://raw.githubusercontent.com/...)` image inside a `## ` feature
  block that survives extraction verbatim (guard test green).
- `release-notes.py --version X.Y.Z` output for an illustrated section equals
  the aggregate slice including images (guard test).
- The live release body on GitHub renders the images (checked like the existing
  single-heading audit after publish).
- No illustration contains real user data; file sizes and the per-release count
  respect the budget - the count, size, URL and committed-asset rules are
  machine-guarded by the audit case in `release-notes-test.py`; the privacy
  rule stays a review-stage check named in the bump PR.
