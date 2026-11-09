#!/usr/bin/env python3
"""Extract one version's own section from the aggregate release-notes document.

`docs/NATIVE_DESKTOP_RELEASE_NOTES.md` keeps every preview newest-first and
separates the versions with horizontal rules. Publishing that document as a
release body repeats earlier versions in each GitHub release, so a release body
is one extracted section selected by the released tag.
"""
import argparse
from pathlib import Path
import re
import sys

VERSION_PATTERN = re.compile(r'(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)')
HEADING_PATTERN = re.compile(r'^# +Workspace Native +(' + VERSION_PATTERN.pattern + r')(?![0-9.]).*$')
SEPARATOR_PATTERN = re.compile(r'-{3,}')
DEFAULT_NOTES = Path(__file__).resolve().parents[3] / 'docs/NATIVE_DESKTOP_RELEASE_NOTES.md'


def sections(text):
    """Return [(version, section)] in document order for one aggregate document."""
    headings = []
    offset = 0
    for line in text.splitlines(keepends=True):
        if (match := HEADING_PATTERN.match(line.rstrip('\r\n'))):
            headings.append((offset, match))
        offset += len(line)
    found = []
    for index, (start, match) in enumerate(headings):
        stop = headings[index + 1][0] if index + 1 < len(headings) else len(text)
        kept = []
        for line in text[start:stop].splitlines():
            if SEPARATOR_PATTERN.fullmatch(line.strip()):
                break
            kept.append(line)
        found.append((match.group(1), '\n'.join(kept).strip('\n') + '\n'))
    return found


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--version', required=True, help='released tag, a stable X.Y.Z')
    parser.add_argument('--notes-file', type=Path, default=DEFAULT_NOTES)
    parser.add_argument('--out', type=Path, help='write the section here; stdout when omitted')
    args = parser.parse_args()
    if not VERSION_PATTERN.fullmatch(args.version):
        parser.error('version must be a stable X.Y.Z')
    text = args.notes_file.read_text(encoding='utf-8')
    catalog = sections(text)
    selected = [section for version, section in catalog if version == args.version]
    if not selected:
        available = ', '.join(version for version, _ in catalog) or 'none'
        raise SystemExit(f'No release-notes section for {args.version} in {args.notes_file}; available: {available}')
    if len(selected) > 1:
        raise SystemExit(f'Duplicate release-notes sections for {args.version} in {args.notes_file}')
    section = selected[0]
    if args.out:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(section, encoding='utf-8')
        print(f'Wrote the {args.version} release notes to {args.out}', file=sys.stderr)
    else:
        sys.stdout.write(section)


if __name__ == '__main__':
    main()
