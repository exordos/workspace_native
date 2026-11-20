#!/bin/sh
# Run from the repository root, as the Linux workflow does after checkout.
set -eu

tracked=$(git ls-files -- ':(top,glob)*.log' ':(top)err.txt' ':(top)Testing')
if [ -n "$tracked" ]; then
    printf 'Generated root artifacts are tracked:\n%s\n' "$tracked" >&2
    exit 1
fi

for scratch in build-configure.log err.txt Testing/Temporary/LastTest.log; do
    if ! git -c core.excludesFile=/dev/null check-ignore -q --no-index -- "$scratch"; then
        printf 'Expected Git to ignore %s\n' "$scratch" >&2
        exit 1
    fi
done
