# Workspace Native

- Read `docs/PROJECT.md` and `docs/NATIVE_DESKTOP_DEVELOPMENT.md` before changes.
- The product UI uses C++17 and Qt Quick. Preserve the Workspace API contracts,
  UUIDs, account/project ownership and pinned desktop presentation.
- Finish Linux parity first. The configurable disk cache defaults to 10 GB.
- Follow `docs/NATIVE_DESKTOP_DEVELOPMENT.md#versioning-and-releases`: numeric
  tags without `v`; MAJOR for loss of compatibility or major architecture changes,
  MINOR for compatible new features, PATCH for bug fixes and small corrections.
  Choose the next version from the latest published release and actual release
  scope; issue, milestone and roadmap versions are provisional, not bump targets.
  Explain the base release and bump level in any PR that changes the version.
- Keep tests and application runtime independent of local developer paths.
- Keep upstream assets/fixtures and their license/provenance intact.
- Document changes under `docs`. The README must stay accurate and current, but
  edit it only when there is a real need: it is a polished description of the
  project for readers, with no technical or implementation details.
- Use `gh` for GitHub operations. Preserve unrelated changes and add regression
  coverage for confirmed defects. Report local, live and platform checks separately.
- Apply the [architecture](docs/standards/architecture.md),
  [testing](docs/standards/testing.md), [documentation](docs/standards/documentation.md)
  and [diagnostics](docs/standards/diagnostics.md) standards. Record
  cross-feature decisions and temporary exceptions in a reviewed decision record.
