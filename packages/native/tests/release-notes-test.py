"""A GitHub release must carry only its own version's notes, never the aggregate."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

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


if __name__ == '__main__':
    unittest.main()
