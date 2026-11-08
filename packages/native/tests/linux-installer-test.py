"""Exercise the real installer in a private HOME, including a two-version switch."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parents[1]/'scripts/install-linux.sh'


class LinuxInstallerTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='cassi-native-install-')
        self.base = Path(self.temporary.name)
        self.home = self.base/"home space's $literal"
        self.home.mkdir()
        self.data = self.home/'data space'
        self.env = dict(os.environ, HOME=str(self.home), XDG_DATA_HOME=str(self.data))
        self.root = self.data/'workspace-native-install'

    def tearDown(self):
        self.temporary.cleanup()

    def package(self, version):
        source = self.base/('package-' + version)
        (source/'bin').mkdir(parents=True)
        (source/'lib').mkdir()
        (source/'VERSION').write_text(version + '\n')
        binary = source/'bin/workspace-native'
        binary.write_text('#!/bin/sh\nif [ -n "${NATIVE_INSTALL_LAUNCH_RESULT:-}" ]; then printf "%s\\n" "' + version + '" > "$NATIVE_INSTALL_LAUNCH_RESULT"; fi\nprintf "%s\\n" "' + version + '" "$@"\n')
        binary.chmod(0o755)
        (source/'lib/test.so.1').write_text(version)
        (source/'lib/test.so').symlink_to('test.so.1')
        shutil.copy2(SCRIPT, source/'install.sh')
        return source

    def install(self, source):
        return subprocess.run(['bash', str(source/'install.sh'), '--no-launch'],
                              env=self.env, capture_output=True, text=True, timeout=15)

    def test_real_install_switch_preserves_previous_cache_and_literal_paths(self):
        cache = self.data/'Exordos/Workspace Native Preview/cache-sentinel'
        cache.parent.mkdir(parents=True)
        cache.write_bytes(b'private cached messages')
        for version in ('0.1.0', '0.1.1'):
            result = self.install(self.package(version))
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(os.readlink(self.root/'current'), 'versions/' + version)
            self.assertEqual((self.root/'current/VERSION').read_text(), version + '\n')
            self.assertTrue((self.root/'current/lib/test.so').is_symlink())
            launch = subprocess.run([str(self.home/'.local/bin/workspace-native'), 'arg with spaces', '$literal'],
                                    env=self.env, capture_output=True, text=True, check=True)
            self.assertEqual(launch.stdout.splitlines(), [version, 'arg with spaces', '$literal'])
            self.assertFalse((self.root/'.install-lock').exists())
        self.assertEqual((self.root/'versions/0.1.0/VERSION').read_text(), '0.1.0\n')
        self.assertEqual(cache.read_bytes(), b'private cached messages')
        self.assertIn('# Workspace Native managed installation',
                      (self.data/'applications/com.exordos.workspace.native.preview.desktop').read_text())

    def test_desktop_entry_resolves_literal_home_using_actual_glib(self):
        import ctypes
        gio = ctypes.CDLL('libgio-2.0.so.0')
        objects = ctypes.CDLL('libgobject-2.0.so.0')
        gio.g_desktop_app_info_new_from_filename.argtypes = [ctypes.c_char_p]
        gio.g_desktop_app_info_new_from_filename.restype = ctypes.c_void_p
        gio.g_app_launch_context_new.restype = ctypes.c_void_p
        gio.g_app_launch_context_setenv.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p]
        gio.g_app_info_launch.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p]
        gio.g_app_info_launch.restype = ctypes.c_int
        objects.g_object_unref.argtypes = [ctypes.c_void_p]
        self.assertEqual(self.install(self.package('0.1.0')).returncode, 0)
        desktop = self.data/'applications/com.exordos.workspace.native.preview.desktop'
        application = gio.g_desktop_app_info_new_from_filename(os.fsencode(desktop))
        self.assertTrue(application, 'The real GLib desktop parser rejected the installed desktop entry')
        try:
            import time
            context = gio.g_app_launch_context_new()
            self.assertTrue(context)
            marker = self.base/'launched-from-desktop'
            try:
                gio.g_app_launch_context_setenv(context, b'NATIVE_INSTALL_LAUNCH_RESULT', os.fsencode(marker))
                self.assertTrue(gio.g_app_info_launch(application, None, context, None))
                deadline = time.monotonic() + 3
                while not marker.exists() and time.monotonic() < deadline:
                    time.sleep(0.02)
                self.assertEqual(marker.read_text(), '0.1.0\n')
            finally:
                objects.g_object_unref(context)
        finally:
            objects.g_object_unref(application)

    def test_existing_foreign_launcher_is_preserved(self):
        launcher = self.home/'.local/bin/workspace-native'
        launcher.parent.mkdir(parents=True)
        launcher.write_text('foreign application\n')
        result = self.install(self.package('0.1.0'))
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(launcher.read_text(), 'foreign application\n')
        self.assertFalse((self.root/'current').exists())

    def test_duplicate_and_parallel_install_do_not_switch_existing_version(self):
        source = self.package('0.1.0')
        self.assertEqual(self.install(source).returncode, 0)
        self.assertNotEqual(self.install(source).returncode, 0)
        self.assertEqual(os.readlink(self.root/'current'), 'versions/0.1.0')
        (self.root/'.install-lock').mkdir()
        self.assertNotEqual(self.install(self.package('0.1.1')).returncode, 0)
        self.assertEqual(os.readlink(self.root/'current'), 'versions/0.1.0')
        (self.root/'.install-lock').rmdir()
        self.assertFalse((self.root/'versions/0.1.1').exists())

    def test_failed_launcher_preparation_keeps_old_version_and_can_retry(self):
        self.assertEqual(self.install(self.package('0.1.0')).returncode, 0)
        # The real launcher preparation fails before any commit is allowed.
        launcher = self.home/'.local/bin/workspace-native'
        blocker = Path(str(launcher) + '.new')
        blocker.mkdir()
        result = self.install(self.package('0.1.1'))
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(os.readlink(self.root/'current'), 'versions/0.1.0')
        self.assertFalse((self.root/'versions/0.1.1').exists())
        self.assertFalse((self.root/'.install-lock').exists())
        self.assertTrue(blocker.is_dir())
        blocker.rmdir()
        retry = self.install(self.base/'package-0.1.1')
        self.assertEqual(retry.returncode, 0, retry.stderr)
        self.assertEqual(os.readlink(self.root/'current'), 'versions/0.1.1')

    def test_symlink_install_root_does_not_modify_target(self):
        foreign = self.base/'foreign'
        foreign.mkdir()
        self.data.mkdir()
        self.root.symlink_to(foreign, target_is_directory=True)
        self.assertNotEqual(self.install(self.package('0.1.0')).returncode, 0)
        self.assertEqual(list(foreign.iterdir()), [])


if __name__ == '__main__':
    unittest.main()
