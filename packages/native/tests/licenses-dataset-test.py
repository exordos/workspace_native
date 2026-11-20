"""Check the Licenses page catalog against build versions and a deployed package inventory."""
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
            [sys.executable, str(SCRIPT), '--out', str(out), '--component', 'qt6-base=6.11.2',
             '--component', 'md4c=0.5.3', '--component', 'sqlite3=3.50.1',
             '--component', 'qtkeychain=0.15.0', '--component', 'openssl=3.5.6'],
            capture_output=True, text=True, timeout=20)
        assert result.returncode == 0, result.stderr
        return out.read_bytes()


class LicensesDatasetTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.raw = generate()
        cls.entries = json.loads(cls.raw)

    def test_packaged_inventory_supplies_versions_and_excludes_absent_components(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            manifest = root / 'bundled-libraries.json'
            output = root / 'licenses.json'
            versions = {'qt6-base': '6.10.2+dfsg-7', 'qt6-declarative': '6.10.2+dfsg-3',
                        'openssl': '3.5.5-1ubuntu3.7', 'qtkeychain': '0.15.0-2',
                        'aom': '3.13.1-2ubuntu0.1'}
            records = {name: {'sourcePackage': name, 'sourceVersion': version,
                              'ubuntuSourcePage': 'https://launchpad.net/ubuntu/+source/' + name}
                       for name, version in versions.items()}
            records['libqt6gui6'] = records['qt6-base']
            manifest.write_text(json.dumps({'schema': 'native-linux-bundled-licenses-v1', 'packages': records}))
            subprocess.run([sys.executable, SCRIPT, '--inventory', manifest, '--out', output], check=True)
            rows = {row['name']: row['version'] for row in json.loads(output.read_text())}
            self.assertEqual(rows, {'Qt 6 QtBase': versions['qt6-base'], 'Qt 6 QtDeclarative': versions['qt6-declarative'],
                                    'OpenSSL': versions['openssl'], 'Qt6Keychain': versions['qtkeychain'],
                                    'aom': versions['aom']})

    def test_optional_engine_rows_use_independently_recorded_versions(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            manifest, output = root / 'inventory.json', root / 'page.json'
            records = {'engine': {'sourcePackage': 'qt6-webengine', 'sourceVersion': '6.10.2-4'},
                       'browser': {'sourcePackage': 'chromium', 'sourceVersion': '134.0.1-2'}}
            manifest.write_text(json.dumps({'schema': 'native-linux-bundled-licenses-v1', 'packages': records}))
            subprocess.run([sys.executable, SCRIPT, '--inventory', manifest, '--out', output], check=True)
            self.assertEqual({row['name']: row['version'] for row in json.loads(output.read_text())},
                             {'Qt 6 QtWebEngine': '6.10.2-4', 'Chromium': '134.0.1-2'})

    def test_two_runs_are_byte_identical(self):
        self.assertEqual(self.raw, generate())

    def test_entry_shape_is_the_five_key_contract(self):
        self.assertEqual(len(self.entries), 5)
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

    def test_build_catalog_contains_only_detected_components(self):
        self.assertEqual({entry['name']: entry['version'] for entry in self.entries}, {
            'Qt 6 QtBase': '6.11.2', 'MD4C': '0.5.3', 'SQLite': '3.50.1',
            'Qt6Keychain': '0.15.0', 'OpenSSL': '3.5.6'})


if __name__ == '__main__':
    unittest.main(verbosity=2)
