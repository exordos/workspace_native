"""A source version bump must never relabel an older configured binary build."""
import os
import runpy
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


class LinuxPackageTest(unittest.TestCase):
    def test_displayed_transitive_source_versions_match_the_installed_inventory(self):
        verify = runpy.run_path(str(Path(__file__).with_name("verify-installed-runtime.py")))["verify_license_catalog"]
        packages = {"openssl": {"sourcePackage": "openssl", "sourceVersion": "3.5.5-1"},
                    "aom": {"sourcePackage": "aom", "sourceVersion": "3.13.1-2"},
                    "aom-older": {"sourcePackage": "aom", "sourceVersion": "3.13.1-1"}}
        rows = [{"name": "OpenSSL", "version": "3.5.5-1"}, {"name": "aom", "version": "3.13.1-2"},
                {"name": "aom", "version": "3.13.1-1"}]
        verify(rows, packages)
        rows[1]["version"] = "wrong"
        with self.assertRaisesRegex(AssertionError, "License catalog drift"):
            verify(rows, packages)

    def test_configured_build_version_is_checked_before_install(self):
        script = Path(os.environ.get('NATIVE_PACKAGE_SCRIPT_UNDER_TEST',
            Path(__file__).resolve().parents[1]/'scripts/package-linux.py'))
        with tempfile.TemporaryDirectory(prefix='cassi-native-package-version-') as temporary:
            base = Path(temporary)
            source = base/'source'; source.mkdir()
            (source/'CMakeLists.txt').write_text('project(WorkspaceNative VERSION 0.1.1 LANGUAGES CXX)\n')
            build = base/'build'; build.mkdir()
            (build/'CMakeCache.txt').write_text(f'WorkspaceNative_SOURCE_DIR:STATIC={source}\n'
                'CMAKE_PROJECT_VERSION:STATIC=0.1.0\nCMAKE_BUILD_TYPE:STRING=Release\n')
            tools = base/'tools'; tools.mkdir()
            marker = base/'install-was-called'
            cmake = tools/'cmake'
            cmake.write_text('#!/bin/sh\ntouch "$INSTALL_MARKER"\nexit 99\n')
            cmake.chmod(0o755)
            env = dict(os.environ, PATH=str(tools) + ':' + os.environ['PATH'], INSTALL_MARKER=str(marker))
            result = subprocess.run([sys.executable, str(script), '--build-dir', str(build),
                '--output-dir', str(base/'output'), '--version', '0.1.1'],
                env=env, capture_output=True, text=True, timeout=10)
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse(marker.exists(), 'An old build reached the installer under a newer source version')
            self.assertIn('Configured build version mismatch', result.stderr)


if __name__ == '__main__':
    unittest.main()
