# Maintaining the native documentation

[PROJECT.md](../PROJECT.md) links to the current build guide, architecture,
diagnostics and feature contracts. Put normative behavior in the owning feature
document. Keep dated measurements, fixture revisions and failure evidence in
the corresponding acceptance record. Preserve old links when reorganizing
evidence. The [development history](../CURRENT_DEVELOPMENT.md) records earlier
outcomes.

When introducing a feature, start with the [feature template](feature-template.md)
and capture the following information before writing code. Reuse an existing feature document when extending it.

| Section | What the reader needs |
| --- | --- |
| User entry and owner | The supported action, observable result and source paths |
| Dependencies | Inputs, stable contracts and storage or network boundaries |
| Lifecycle | Account/project scope, cancellation, retries and cleanup |
| Limits and errors | Budgets, error states and effects on stored data |
| Verification | Focused command and required real UI, package or service evidence |

Use a [decision record](../decisions/template.md) for a material dependency,
public format or cross-feature ownership change. Mark its status and link to a
successor when superseded. In a PR, list the affected contract and which parts
of it have been verified. Validate referenced paths and runnable examples from
a fresh checkout.
