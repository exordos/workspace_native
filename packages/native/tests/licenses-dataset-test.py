"""The native licenses dataset must be deterministic, well-shaped and honest.

`scripts/collect-native-licenses.py` emits the third-party dependency dataset
the native client embeds as `qrc:/workspace/licenses/licenses.json` (issue #46,
docs/NATIVE_DESKTOP_LICENSES.md). This check runs the generator directly and
pins the contract the Licenses page relies on: byte-stable output, the
five-key entry shape, the presence of the client's real dependencies, the
sorted order, and a license-type census that exercises the top-8 chip cap.
It is deliberately not registered in CTest so the CTest group count stays
stable; run it with `python3 packages/native/tests/licenses-dataset-test.py`.
"""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / 'scripts/collect-native-licenses.py'


def generate():
    with tempfile.TemporaryDirectory(prefix='cassi-licenses-') as temporary:
        out = Path(temporary) / 'licenses.json'
        result = subprocess.run(
            [sys.executable, str(SCRIPT), '--out', str(out)],
            capture_output=True, text=True, timeout=20)
        assert result.returncode == 0, result.stderr
        return out.read_bytes()


class LicensesDatasetTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.raw = generate()
        cls.entries = json.loads(cls.raw)

    def test_two_runs_are_byte_identical(self):
        self.assertEqual(self.raw, generate())

    def test_entry_shape_is_the_five_key_contract(self):
        self.assertGreaterEqual(len(self.entries), 16)
        for entry in self.entries:
            self.assertEqual(sorted(entry),
                ['license', 'name', 'publisher', 'repository', 'version'])
            for key, value in entry.items():
                self.assertIsInstance(value, str, key)
            for key in ('name', 'version', 'license', 'publisher'):
                self.assertTrue(entry[key].strip(), entry)

    def test_the_clients_own_dependencies_are_listed(self):
        names = ' \n'.join(entry['name'] for entry in self.entries).lower()
        for needle in ('qt', 'md4c', 'sqlite', 'keychain', 'openssl'):
            self.assertIn(needle, names)

    def test_entries_are_sorted_for_stable_rows(self):
        keys = [(entry['name'], entry['version']) for entry in self.entries]
        self.assertEqual(keys, sorted(keys))

    def test_at_least_one_plain_text_package_cell_exists(self):
        self.assertTrue(any(entry['repository'] == '' for entry in self.entries))

    def license_type_census(self):
        counts = {}
        order = []
        for entry in self.entries:
            if entry['license'] not in counts:
                counts[entry['license']] = 0
                order.append(entry['license'])
            counts[entry['license']] += 1
        ordered = sorted(order, key=lambda license_name: -counts[license_name])
        return ordered, counts

    def test_top_eight_chip_census_matches_the_dataset(self):
        ordered, counts = self.license_type_census()
        self.assertGreater(len(ordered), 8,
            'the dataset must exceed eight license types to exercise the cap')
        self.assertEqual(ordered[:8], ['MIT', 'LGPL-3.0-only', 'BSD-3-Clause',
            'FTL', 'Apache-2.0', 'LGPL-2.1-or-later', 'blessing', 'Zlib'])
        self.assertEqual([counts[name] for name in ordered[:8]], [5, 3, 2, 1, 1, 1, 1, 1])


if __name__ == '__main__':
    unittest.main(verbosity=2)
