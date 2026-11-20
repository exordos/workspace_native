#!/usr/bin/env python3
"""Build the Licenses page catalog from verified source-package versions."""

import argparse
import json
from pathlib import Path

# Display metadata, keyed by source package. Versions always come from the
# deployed distribution's inventory or the libraries detected by CMake.
COMPONENTS = {
    'chromium': ('Chromium', 'BSD-3-Clause',
        'https://github.com/chromium/chromium', 'The Chromium Authors'),
    'expat': ('Expat', 'MIT',
        'https://github.com/libexpat/libexpat', 'libexpat contributors'),
    'freetype': ('FreeType', 'FTL',
        'https://git.savannah.gnu.org/cgit/freetype/freetype2.git', 'FreeType Contributors'),
    'harfbuzz': ('HarfBuzz', 'MIT',
        'https://github.com/harfbuzz/harfbuzz', 'The HarfBuzz Project'),
    'icu': ('ICU (International Components for Unicode)', 'MIT',
        'https://github.com/unicode-org/icu', 'Unicode, Inc.'),
    'md4c': ('MD4C', 'MIT',
        'https://github.com/mity/md4c', 'Victor Zvegintcev'),
    'openssl': ('OpenSSL', 'Apache-2.0',
        'https://github.com/openssl/openssl', 'OpenSSL Software Foundation'),
    'pcre2': ('PCRE2', 'BSD-3-Clause',
        'https://github.com/PCRE2Project/pcre2', 'Philip Hazel'),
    'qt6-base': ('Qt 6 QtBase', 'LGPL-3.0-only',
        'https://github.com/qt/qtbase', 'The Qt Company'),
    'qt6-declarative': ('Qt 6 QtDeclarative', 'LGPL-3.0-only',
        'https://github.com/qt/qtdeclarative', 'The Qt Company'),
    'qt6-webengine': ('Qt 6 QtWebEngine', 'LGPL-3.0-only',
        'https://github.com/qt/qtwebengine', 'The Qt Company'),
    'qtkeychain': ('Qt6Keychain', 'LGPL-2.1-or-later',
        'https://github.com/frankosterfeld/qtkeychain', 'Frank Osterfeld'),
    'sqlite3': ('SQLite', 'blessing',
        '', 'SQLite Development Team'),
    'libxcb': ('XCB Protocol Library', 'MIT',
        'https://gitlab.freedesktop.org/xcb/libxcb', 'The X.Org Foundation'),
    'zlib': ('Zlib', 'Zlib',
        'https://github.com/madler/zlib', 'Jean-loup Gailly and Mark Adler'),
    'libpng1.6': ('libpng', 'libpng-2.0',
        'https://github.com/glennrp/libpng', 'Glenn Randers-Pehrson'),
    'qt6-svg': ('Qt 6 QtSvg', 'LGPL-3.0-only',
        'https://github.com/qt/qtsvg', 'The Qt Company'),
    'qt6-multimedia': ('Qt 6 QtMultimedia', 'LGPL-3.0-only',
        'https://github.com/qt/qtmultimedia', 'The Qt Company'),
    'qt6-websockets': ('Qt 6 QtWebSockets', 'LGPL-3.0-only',
        'https://github.com/qt/qtwebsockets', 'The Qt Company'),
    'qt6-wayland': ('Qt 6 QtWayland', 'LGPL-3.0-only',
        'https://github.com/qt/qtwayland', 'The Qt Company'),
    'qt6-imageformats': ('Qt 6 QtImageFormats', 'LGPL-3.0-only',
        'https://github.com/qt/qtimageformats', 'The Qt Company'),
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    inputs = parser.add_mutually_exclusive_group(required=True)
    inputs.add_argument('--inventory', type=Path, help='bundled-libraries.json from the deployed prefix')
    inputs.add_argument('--component', action='append', metavar='SOURCE=VERSION',
                        help='library and version detected for an unpackaged build')
    parser.add_argument('--qt-sqlite', action='store_true',
                        help='include SQLite metadata for version detection by the active Qt SQL driver')
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    if args.inventory:
        manifest = json.loads(args.inventory.read_text(encoding='utf-8'))
        if manifest['schema'] != 'native-linux-bundled-licenses-v1' or not manifest['packages']:
            parser.error('Expected a nonempty bundled Linux package inventory')
        records = manifest['packages'].values()
    else:
        records = [{'sourcePackage': 'sqlite3', 'sourceVersion': ''}] if args.qt_sqlite else []
        for component in args.component:
            source, separator, version = component.partition('=')
            if not separator or not source or not version:
                parser.error('Expected SOURCE=VERSION from the build')
            records.append({'sourcePackage': source, 'sourceVersion': version})

    entries = {}
    for record in records:
        source, version = record['sourcePackage'], record['sourceVersion']
        name, license_id, repo, publisher = COMPONENTS.get(source,
            (source, 'See bundled notice', record.get('ubuntuSourcePage', ''), ''))
        entries[(name, version)] = {'name': name, 'version': version, 'license': license_id,
                                    'repository': repo, 'publisher': publisher}
    payload = [entries[key] for key in sorted(entries)]
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(payload, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
