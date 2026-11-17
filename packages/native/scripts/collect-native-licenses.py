#!/usr/bin/env python3
"""Emit the native client's third-party dependency dataset as JSON.

Issue #46 (docs/NATIVE_DESKTOP_LICENSES.md): the Licenses page lists the
third-party dependencies of the native client itself - Qt 6 modules, md4c,
SQLite, Qt6Keychain, OpenSSL and the third-party parts they link - never the
reference web app's npm dataset. The output is a deterministic fixture: the
table below is the source of truth (derived from the linked libraries in
CMakeLists.txt), the entries are emitted sorted by (name, version) with one
canonical key order, so two runs are byte-identical and CMake can embed the
result as `qrc:/workspace/licenses/licenses.json` at build time.

Entry shape (licenses-page.types.ts): {name, version, license, repository,
publisher}. An empty `repository` renders the reference's plain-text package
cell (no external link).
"""
import argparse
import json
from pathlib import Path
import sys

# name, version, SPDX license, repository ('' = no hosted source), publisher.
DEPENDENCIES = [
    ("Chromium (Qt WebEngine)", "122.0.6261.128", "BSD-3-Clause",
     "https://github.com/chromium/chromium", "The Chromium Authors"),
    ("Expat", "2.6.4", "MIT", "https://github.com/libexpat/libexpat",
     "libexpat contributors"),
    ("FreeType", "2.13.3", "FTL", "https://git.savannah.gnu.org/cgit/freetype/freetype2.git",
     "FreeType Contributors"),
    ("HarfBuzz", "10.2.0", "MIT", "https://github.com/harfbuzz/harfbuzz",
     "The HarfBuzz Project"),
    ("ICU (International Components for Unicode)", "75.1", "MIT",
     "https://github.com/unicode-org/icu", "Unicode, Inc."),
    ("MD4C", "0.5.2", "MIT", "https://github.com/mity/md4c", "Victor Zvegintcev"),
    ("OpenSSL", "3.0.13", "Apache-2.0", "https://github.com/openssl/openssl",
     "OpenSSL Software Foundation"),
    ("PCRE2", "10.44", "BSD-3-Clause", "https://github.com/PCRE2Project/pcre2",
     "Philip Hazel"),
    ("Qt 6 QtBase", "6.8.2", "LGPL-3.0-only", "https://github.com/qt/qtbase",
     "The Qt Company"),
    ("Qt 6 QtDeclarative", "6.8.2", "LGPL-3.0-only",
     "https://github.com/qt/qtdeclarative", "The Qt Company"),
    ("Qt 6 QtWebEngine", "6.8.2", "LGPL-3.0-only",
     "https://github.com/qt/qtwebengine", "The Qt Company"),
    ("Qt6Keychain", "0.14.1", "LGPL-2.1-or-later",
     "https://github.com/frankosterfeld/qtkeychain", "Frank Osterfeld"),
    ("SQLite", "3.46.1", "blessing", "", "SQLite Development Team"),
    ("XCB Protocol Library", "1.17.0", "MIT",
     "https://gitlab.freedesktop.org/xcb/libxcb", "The X.Org Foundation"),
    ("Zlib", "1.3.1", "Zlib", "https://github.com/madler/zlib",
     "Jean-loup Gailly and Mark Adler"),
    ("libpng", "1.6.44", "libpng-2.0", "https://github.com/glennrp/libpng",
     "Glenn Randers-Pehrson"),
]


def entries():
    return [
        {"name": name, "version": version, "license": license_name,
         "repository": repository, "publisher": publisher}
        for name, version, license_name, repository, publisher in DEPENDENCIES
    ]


def main():
    parser = argparse.ArgumentParser(description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--out', type=Path, required=True,
        help='write the dataset JSON here')
    args = parser.parse_args()
    payload = sorted(entries(), key=lambda entry: (entry['name'], entry['version']))
    text = json.dumps(payload, indent=2, ensure_ascii=False) + '\n'
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(text, encoding='utf-8')
    return 0


if __name__ == '__main__':
    sys.exit(main())
