# Diagnostics and support data

The existing [rotating log provider](../NATIVE_DESKTOP_DIAGNOSTICS.md) keeps
the active file and one previous file, with a 1 MiB rotation threshold and a
32,768-character line limit. The [diagnostics page](../NATIVE_DESKTOP_DIAGNOSTICS_PAGE.md)
has a separate 500-entry session ring. Reuse their counters for support work.
Free-form log lines can contain personal data and are unsuitable for automatic
attachment to a report.

Collect support diagnostics using a schema of allowed fields. Encode event
classes, counts, durations and build information. Exclude message and draft
text, account identifiers, hostnames, file paths, URL parameters, credentials,
key material and request bodies. Validate the serialized artifact again at
export. Use synthetic secrets in nested values, URLs and paths in regression
tests. Keep filenames independent of user data.

Keep local collection available offline and before authentication. Show the
exact bytes that will be exported, allow the user to exclude optional sections
and preserve explicit cancellation. An explicitly chosen destination and
user review are required for any transmission. Public bug reports should
contain a sanitized description and a report-local identifier. Use a private
administrator or support conversation for sensitive follow-up.

The [crash reporting decision](../decisions/0001-local-support-report.md)
describes the data budget and the scope of native dump collection.
