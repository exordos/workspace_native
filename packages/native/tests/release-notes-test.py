"""A GitHub release must carry only its own version's notes, never the aggregate."""
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest

RAWMARK = 'https://raw.githubusercontent.com/exordos/workspace_native/master/docs/images/releases/'
IMAGE_LINE = re.compile(r'^!\[([^\]]*)\]\((\S+)\)$')
SLUG_PATTERN = re.compile(r'[a-z0-9][a-z0-9-]*')
MAX_IMAGES_PER_SECTION = 6
IMAGE_BUDGET = {'png': 500 * 1024, 'gif': 5 * 1024 * 1024}


def audit_illustrations(section, version, repo_root):
    """Return illustration contract violations in one release-notes section.

    Per docs/NATIVE_DESKTOP_ILLUSTRATED_RELEASE_NOTES.md every image must stand
    alone on its line, carry descriptive alt text, use an absolute raw URL
    under docs/images/releases/<version>/ with a lowercase-hyphen slug, be
    committed in the repository tree, and respect the per-release count and the
    per-file size budget.
    """
    problems = []
    images = 0
    for index, line in enumerate(section.splitlines(), start=1):
        if '![' not in line:
            continue
        stripped = line.strip()
        if not (match := IMAGE_LINE.fullmatch(stripped)):
            problems.append(f'line {index}: an illustration must stand alone in its paragraph')
            continue
        images += 1
        alt, url = match.group(1), match.group(2)
        if not alt.strip():
            problems.append(f'line {index}: an illustration needs descriptive alt text')
        prefix = RAWMARK + version + '/'
        if not url.startswith(prefix):
            problems.append(f'line {index}: {url} is not an absolute raw URL under docs/images/releases/{version}/')
            continue
        remainder = url[len(prefix):]
        slug, _, extension = remainder.rpartition('.')
        if '/' in slug or extension not in IMAGE_BUDGET or not SLUG_PATTERN.fullmatch(slug):
            problems.append(f'line {index}: {url} must be a <slug>.png or <slug>.gif with a lowercase-hyphen slug')
            continue
        if images > MAX_IMAGES_PER_SECTION:
            problems.append(f'line {index}: a release carries at most {MAX_IMAGES_PER_SECTION} illustrations')
        asset = Path(repo_root) / 'docs/images/releases' / version / f'{slug}.{extension}'
        if not asset.is_file():
            problems.append(f'line {index}: {asset} is not committed to the repository')
        elif asset.stat().st_size > IMAGE_BUDGET[extension]:
            problems.append(f'line {index}: {asset.name} exceeds the {IMAGE_BUDGET[extension]} byte budget')
    return problems

AGGREGATE = (
    '# Workspace Native 0.2.0 - Linux development preview\n'
    '\n'
    'Drafts now survive restart.\n'
    '\n'
    'Existing installations update automatically.\n'
    '\n'
    '[Full changelog: 0.1.6...0.2.0](https://example.invalid/compare/0.1.6...0.2.0)\n'
    '\n'
    '---\n'
    '\n'
    '# Workspace Native 0.1.6 - Linux development preview\n'
    '\n'
    'Image previews are bounded.\n'
    '\n'
    '---\n'
    '\n'
    '# Workspace Native 0.1.3 - Linux development preview\n'
    '\n'
    'Standalone image paragraphs show previews.\n')


def run_script(script, *arguments):
    return subprocess.run([sys.executable, str(script), *arguments], capture_output=True, text=True, timeout=10)


class ReleaseNotesTest(unittest.TestCase):
    def setUp(self):
        self.script = Path(os.environ.get('NATIVE_RELEASE_NOTES_SCRIPT_UNDER_TEST',
            Path(__file__).resolve().parents[1]/'scripts/release-notes.py'))

    def extract(self, text, version):
        with tempfile.TemporaryDirectory(prefix='cassi-release-notes-') as temporary:
            notes = Path(temporary)/'notes.md'
            notes.write_text(text)
            result = run_script(self.script, '--version', version, '--notes-file', str(notes))
            self.assertEqual(result.returncode, 0, result.stderr)
            return result.stdout

    def test_a_section_excludes_older_versions(self):
        section = self.extract(AGGREGATE, '0.2.0')
        self.assertIn('Drafts now survive restart.', section)
        self.assertNotIn('0.1.6 - Linux development preview', section)
        self.assertNotIn('Image previews are bounded.', section)

    def test_the_last_section_has_no_separator(self):
        section = self.extract(AGGREGATE, '0.1.3')
        self.assertEqual(section.strip().splitlines()[0],
            '# Workspace Native 0.1.3 - Linux development preview')
        self.assertNotIn('0.1.6', section)

    def test_an_illustrated_section_survives_extraction(self):
        illustrated = (
            '# Workspace Native 0.3.0 - Linux development preview\n'
            '\n'
            'In this release\n'
            '\n'
            '- Chat backgrounds arrived.\n'
            '\n'
            '## Chat backgrounds\n'
            '\n'
            '![Chat background picker over a topic](https://raw.githubusercontent.com/'
            'exordos/workspace_native/master/docs/images/releases/0.3.0/chat-backgrounds.png)\n'
            '\n'
            'What changed: pick a preset or your own image as the background of one chat.\n'
            '\n'
            '[Full changelog from 0.2.0 to 0.3.0](https://example.invalid/compare/0.2.0...0.3.0)\n'
            '\n'
            '---\n'
            '\n')
        section = self.extract(illustrated + AGGREGATE, '0.3.0')
        self.assertIn('## Chat backgrounds', section)
        self.assertIn('![Chat background picker over a topic](https://raw.githubusercontent.com/'
            'exordos/workspace_native/master/docs/images/releases/0.3.0/chat-backgrounds.png)', section)
        self.assertIn('Full changelog', section)
        self.assertNotIn('Drafts now survive restart.', section)

    def test_a_shorter_version_never_matches_a_longer_one(self):
        self.assertNotIn('0.1.30', self.extract(AGGREGATE + '\n---\n\n'
            '# Workspace Native 0.1.30 - Linux development preview\n\nLater work.\n', '0.1.3'))

    def test_an_unknown_version_is_refused_with_the_available_ones(self):
        with tempfile.TemporaryDirectory(prefix='cassi-release-notes-') as temporary:
            notes = Path(temporary)/'notes.md'
            notes.write_text(AGGREGATE)
            result = run_script(self.script, '--version', '9.9.9', '--notes-file', str(notes))
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('No release-notes section for 9.9.9', result.stderr)
            self.assertIn('0.2.0, 0.1.6, 0.1.3', result.stderr)

    def test_the_repository_document_has_one_section_per_released_tag(self):
        document = Path(os.environ.get('NATIVE_RELEASE_NOTES_DOCUMENT_UNDER_TEST',
            Path(__file__).resolve().parents[3]/'docs/NATIVE_DESKTOP_RELEASE_NOTES.md'))
        text = document.read_text()
        headings = [line for line in text.splitlines() if line.startswith('# Workspace Native ')]
        self.assertGreater(len(headings), 1, 'an aggregate document holds several versions')
        for heading in headings:
            version = heading.split()[3]
            section = self.extract(text, version)
            self.assertEqual(section.splitlines()[0], heading)
            for other in headings:
                if other != heading and other.split()[3] != version:
                    self.assertNotIn(other, section, f'{version} repeated another version')


    def test_the_repository_document_respects_the_illustration_audit(self):
        document = Path(os.environ.get('NATIVE_RELEASE_NOTES_DOCUMENT_UNDER_TEST',
            Path(__file__).resolve().parents[3]/'docs/NATIVE_DESKTOP_RELEASE_NOTES.md'))
        text = document.read_text()
        repo_root = document.parent.parent
        for line in text.splitlines():
            if not line.startswith('# Workspace Native '):
                continue
            version = line.split()[3]
            section = self.extract(text, version)
            self.assertEqual(audit_illustrations(section, version, repo_root), [],
                f'{version} carries a non-conforming illustration')

    def test_a_committed_absolute_illustration_passes_the_audit(self):
        with tempfile.TemporaryDirectory(prefix='cassi-release-audit-') as temporary:
            root = Path(temporary)
            assets = root/'docs/images/releases/0.3.0'
            assets.mkdir(parents=True)
            (assets/'chat-backgrounds.png').write_bytes(b'png')
            section = ('# Workspace Native 0.3.0 - Linux development preview\n\n'
                '## Chat backgrounds\n\n'
                '![Chat background picker over a topic](' + RAWMARK + '0.3.0/chat-backgrounds.png)\n')
            self.assertEqual(audit_illustrations(section, '0.3.0', root), [])

    def test_the_audit_flags_relative_urls_inlined_images_and_missing_assets(self):
        with tempfile.TemporaryDirectory(prefix='cassi-release-audit-') as temporary:
            root = Path(temporary)
            relative = audit_illustrations('![picker](docs/images/releases/0.3.0/picker.png)\n', '0.3.0', root)
            self.assertEqual(len(relative), 1)
            self.assertIn('absolute raw URL', relative[0])
            inlined = audit_illustrations('The ![picker](' + RAWMARK + '0.3.0/picker.png) sits inline.\n', '0.3.0', root)
            self.assertEqual(len(inlined), 1)
            self.assertIn('stand alone', inlined[0])
            missing = audit_illustrations('![picker](' + RAWMARK + '0.3.0/picker.png)\n', '0.3.0', root)
            self.assertEqual(len(missing), 1)
            self.assertIn('not committed', missing[0])

    def test_the_audit_flags_empty_alt_text_and_foreign_version_folders(self):
        with tempfile.TemporaryDirectory(prefix='cassi-release-audit-') as temporary:
            root = Path(temporary)
            empty_alt = audit_illustrations('![](' + RAWMARK + '0.3.0/picker.png)\n', '0.3.0', root)
            self.assertIn('descriptive alt text', ' '.join(empty_alt))
            foreign = audit_illustrations('![picker](' + RAWMARK + '0.2.0/picker.png)\n', '0.3.0', root)
            self.assertEqual(len(foreign), 1)
            self.assertIn('under docs/images/releases/0.3.0/', foreign[0])

    def test_the_audit_flags_over_budget_assets(self):
        with tempfile.TemporaryDirectory(prefix='cassi-release-audit-') as temporary:
            root = Path(temporary)
            assets = root/'docs/images/releases/0.3.0'
            assets.mkdir(parents=True)
            (assets/'oversized.png').write_bytes(b'x' * (500 * 1024 + 1))
            (assets/'movie.gif').write_bytes(b'x' * (5 * 1024 * 1024 + 1))
            problems = audit_illustrations(
                '![too big](' + RAWMARK + '0.3.0/oversized.png)\n\n'
                '![too long](' + RAWMARK + '0.3.0/movie.gif)\n', '0.3.0', root)
            self.assertEqual(len(problems), 2)
            self.assertIn('oversized.png', ' '.join(problems))
            self.assertIn('movie.gif', ' '.join(problems))
            for problem in problems:
                self.assertIn('byte budget', problem)

    def test_the_audit_flags_a_seventh_illustration(self):
        with tempfile.TemporaryDirectory(prefix='cassi-release-audit-') as temporary:
            root = Path(temporary)
            assets = root/'docs/images/releases/0.3.0'
            assets.mkdir(parents=True)
            lines = []
            for number in range(MAX_IMAGES_PER_SECTION + 1):
                (assets/f'shot-{number}.png').write_bytes(b'png')
                lines.append(f'![shot {number}](' + RAWMARK + f'0.3.0/shot-{number}.png)')
            problems = audit_illustrations('\n'.join(lines) + '\n', '0.3.0', root)
            self.assertEqual(problems, [f'line {MAX_IMAGES_PER_SECTION + 1}: '
                f'a release carries at most {MAX_IMAGES_PER_SECTION} illustrations'])


if __name__ == '__main__':
    unittest.main()
