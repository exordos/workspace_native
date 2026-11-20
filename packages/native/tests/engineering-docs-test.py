#!/usr/bin/env python3
"""Check local links in current engineering guides without network access."""

import re
import unittest
from pathlib import Path
from urllib.parse import unquote, urlsplit

ROOT = Path(__file__).resolve().parents[3]
DOCS = ROOT / "docs"


def inline_links(markdown):
    # Common guide links can carry a title or put a path with spaces in angle brackets.
    pattern = r"""
        \[[^\]]+\]\(\s*
        (?: <([^>\n]+)> | ([^\s)]+) )
        (?: \s+ (?:"[^"]*"|'[^']*'|\([^()]*\)) )?
        \s*\)
    """
    return [angle or plain for angle, plain in re.findall(pattern, markdown, re.VERBOSE)]


def headings(markdown):
    result = set()
    for line in markdown.splitlines():
        heading = re.match(r"^#{1,6}\s+(.+)", line)
        if heading:
            slug = re.sub(r"[^\w -]", "", heading[1].strip().lower()).replace(" ", "-")
            result.add(slug)
    return result


class EngineeringDocs(unittest.TestCase):
    def test_link_destinations_with_optional_titles(self):
        example = """[one](a.md "notes") [two](<b c.md> 'details') [three](d.md)"""
        self.assertEqual(inline_links(example), ["a.md", "b c.md", "d.md"])

    def test_local_links(self):
        guides = [ROOT / "AGENTS.md", DOCS / "PROJECT.md"]
        guides.extend(sorted((DOCS / "standards").glob("*.md")))
        guides.extend(sorted((DOCS / "decisions").glob("*.md")))
        for guide in guides:
            for target in inline_links(guide.read_text()):
                with self.subTest(guide=guide.name, target=target):
                    parts = urlsplit(target)
                    if parts.scheme or parts.netloc:
                        continue
                    path = (guide.parent / unquote(parts.path)).resolve() if parts.path else guide
                    self.assertTrue(path.is_relative_to(ROOT))
                    self.assertTrue(path.is_file())
                    if parts.fragment and path.suffix == ".md":
                        self.assertIn(unquote(parts.fragment), headings(path.read_text()))

    def test_paths_in_module_map(self):
        page = (DOCS / "standards/architecture.md").read_text()
        paths = re.findall(r"`((?:src|qml)/[a-zA-Z0-9./-]+)`", page)
        self.assertTrue(paths)
        for path in paths:
            with self.subTest(path=path):
                self.assertTrue((ROOT / "packages/native" / path).exists())


if __name__ == "__main__":
    unittest.main()
