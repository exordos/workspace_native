# Workspace Native

- Read `docs/PROJECT.md` and `docs/NATIVE_DESKTOP_DEVELOPMENT.md` before changes.
- The product UI uses C++17 and Qt Quick. Preserve the Workspace API contracts,
  UUIDs, account/project ownership and pinned desktop presentation.
- Finish Linux parity first. The configurable disk cache defaults to 10 GB.
- Follow `docs/NATIVE_DESKTOP_DEVELOPMENT.md#versioning-and-releases`: numeric
  tags without `v`; MAJOR for incompatible public interfaces or very large
  architecture changes, MINOR for new features, PATCH for existing-function fixes.
- Keep tests and application runtime independent of local developer paths.
- Keep upstream assets/fixtures and their license/provenance intact.
- Document changes under `docs`; do not change or add README without approval.
- Use `gh` for GitHub operations. Preserve unrelated changes and add regression
  coverage for confirmed defects. Report local, live and platform checks separately.
