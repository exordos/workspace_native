#!/usr/bin/env python3
"""Generate native presentation metadata from the pinned desktop picker archive."""

import argparse
import base64
import hashlib
import json
from pathlib import Path
import tarfile


VERSION = "4.19.1"
ARCHIVE_SHA256 = "01e95c6f77fcfa212d93d80445a6a13594766a40358440b7c84147bc57baacde"
CATALOG_SHA256 = "c02c9b3ddad6a82e61eb290b86d9b803e00407eca69103908a376d548b308716"
SOURCE_HASHES = {
    "src/data/emojis.json": "001de05839cc490ac775a5445153c9e9a065ad17cc8ac7d4516801a04c78ceb5",
    "src/config/categoryConfig.ts": "0722764800b845a3920c2ffa65044969a95b7e376a10a2bcb1aa520319ef6100",
    "src/config/config.ts": "0d4d205ad01950d68d9520803a72a0dd61aec9bfea0c74e109bf4ebaab599b84",
    "src/dataUtils/suggested.ts": "b8fc66f0edb9a53c9a2332482a7aac4ef07413cff5604d028650741028f3bb99",
    "LICENSE": "03686cc5661b83d5dd4c983f17a3a6d85748e520526fb8dce7e6ede0ff578983",
}
CATEGORIES = [
    "smileys_people", "animals_nature", "food_drink", "travel_places",
    "activities", "objects", "symbols", "flags",
]
HIDDEN = {"2640-fe0f", "2642-fe0f", "2695-fe0f"}
ASSETS = {
    "emoji_picker_navigation.svg": ("src/components/navigation/svg/CategoryNav.svg", "3265e7b66c04740fc8beaae033c5a867cbe4c6c4196da1750094fc03f4774707"),
    "emoji_picker_search.svg": ("src/components/header/Search/svg/magnifier.svg", "db6387bf6b0e61f7b346177595980f83ef9c18d902577696d3ab731072f7e7c9"),
    "emoji_picker_clear.svg": ("src/components/header/Search/svg/times.svg", "28381bdbdd11b49eba6cba7fd1efb689d96de303d8d1443e60b24be333fd563f"),
    "emoji_picker_triangle.svg": ("src/components/body/svg/triangle.svg", "a0d366238d1dc80d40fac8f5c61ae497e7a25229f0c4a48a73b7d530b3fc23c2"),
}


def digest(data):
    return hashlib.sha256(data).hexdigest()


def normalized(sequence):
    return "-".join(format(int(point, 16), "x") for point in sequence.split("-")
                    if int(point, 16) != 0xfe0f)


def glyph(sequence):
    return "".join(chr(int(point, 16)) for point in sequence.split("-"))


def generate(archive_path, catalog_path, lock_path):
    archive_bytes = archive_path.read_bytes()
    if digest(archive_bytes) != ARCHIVE_SHA256:
        raise ValueError("The source archive does not match the pinned desktop package.")
    locked = json.loads(lock_path.read_bytes())["packages"]["node_modules/emoji-picker-react"]
    integrity = "sha512-" + base64.b64encode(hashlib.sha512(archive_bytes).digest()).decode()
    if locked["version"] != VERSION or locked["integrity"] != integrity:
        raise ValueError("The source archive does not match the desktop lockfile integrity.")
    source, assets = {}, {}
    with tarfile.open(archive_path, "r:gz") as archive:
        # Read exact members without extracting archive paths or executing code.
        package = json.loads(archive.extractfile("package/package.json").read())
        if package["name"] != "emoji-picker-react" or package["version"] != VERSION:
            raise ValueError("Unexpected source package identity.")
        for name, expected in SOURCE_HASHES.items():
            source[name] = archive.extractfile("package/" + name).read()
            if digest(source[name]) != expected:
                raise ValueError("Unexpected pinned source content: " + name)
        for output, (name, expected) in ASSETS.items():
            assets[output] = archive.extractfile("package/" + name).read()
            if digest(assets[output]) != expected:
                raise ValueError("Unexpected pinned SVG content: " + name)
    catalog_bytes = catalog_path.read_bytes()
    if digest(catalog_bytes) != CATALOG_SHA256:
        raise ValueError("The backend wire catalog changed; review the mapping before regenerating.")
    catalog = json.loads(catalog_bytes)["workspace"]
    identities = {normalized(sequence): names[0] for sequence, names in catalog.items()}
    data = json.loads(source["src/data/emojis.json"])
    entries, seen, mapped = [], set(), set()
    for category in CATEGORIES:
        for item in data["emojis"][category]:
            base_sequence = item["u"]
            base = identities[normalized(base_sequence)]
            if base in seen:
                raise ValueError("Duplicate source base identity: " + base)
            seen.add(base)
            variants = []
            for sequence in item.get("v", []):
                variant = identities[normalized(sequence)]
                variants.append([variant, glyph(sequence)])
                mapped.add(variant)
            mapped.add(base)
            # Array columns are fixed by schema 1: base, category index, exact
            # display glyph, source search terms, variants, default visibility.
            entries.append([base, CATEGORIES.index(category), glyph(base_sequence), item["n"],
                            variants, base_sequence not in HIDDEN])
    if len(entries) != 1898 or len(mapped) != 3773 or len(catalog) != 3979:
        raise ValueError("Unexpected pinned catalog coverage.")
    metadata = {
        "schema": 1,
        "source": {
            "package": "emoji-picker-react", "version": VERSION,
            "archive": "https://registry.npmjs.org/emoji-picker-react/-/emoji-picker-react-" + VERSION + ".tgz",
            "archive_sha256": ARCHIVE_SHA256, "files_sha256": SOURCE_HASHES,
            "lock_integrity": integrity,
            "wire_catalog_sha256": CATALOG_SHA256,
        },
        "categories": CATEGORIES,
        "skin_tones": ["neutral", "1f3fb", "1f3fc", "1f3fd", "1f3fe", "1f3ff"],
        "entries": entries,
    }
    license_text = (
        "Native emoji picker presentation metadata\n\n"
        "Derived from emoji-picker-react " + VERSION + ", pinned by the desktop package-lock.json.\n"
        "Upstream: https://github.com/ealush/emoji-picker-react\n"
        "The generator verifies the published archive and source SHA-256 hashes.\n"
        "Ordered Unicode/search/category/variant metadata and four SVG assets are reused; no React or remote image runtime is embedded.\n\n"
    ).encode() + source["LICENSE"] + b"\n"
    return (json.dumps(metadata, ensure_ascii=False, separators=(",", ":")) + "\n").encode(), license_text, assets


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", required=True, type=Path,
                        help="Unmodified official emoji-picker-react 4.19.1 npm archive")
    parser.add_argument("--check", action="store_true", help="Verify checked-in outputs without writing")
    args = parser.parse_args()
    resources = Path(__file__).resolve().parents[1] / "resources"
    try:
        metadata, license_text, assets = generate(args.archive, resources / "unicode_emoji.json",
                                                  Path(__file__).resolve().parents[3] / "package-lock.json")
        for name, content in [("emoji_picker.json", metadata), ("emoji_picker.LICENSE", license_text), *assets.items()]:
            target = resources / name
            if args.check:
                if not target.is_file() or target.read_bytes() != content:
                    raise ValueError("Generated output differs: " + name)
            else:
                target.write_bytes(content)
    except (ValueError, KeyError, OSError, tarfile.TarError) as error:
        parser.exit(1, str(error) + "\n")
    print("Pinned picker metadata: 1898 bases, 3773 mapped identities, 206 wire-only identities.")


if __name__ == "__main__":
    main()
