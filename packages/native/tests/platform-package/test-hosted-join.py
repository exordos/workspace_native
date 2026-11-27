"""An ambiguous macOS accessibility reply still requires the real meeting's media reports."""
import importlib.util
import pathlib
import subprocess
import sys
import unittest
import unittest.mock


path = pathlib.Path(__file__).with_name("desktop-account-acceptance.py")
spec = importlib.util.spec_from_file_location("desktop_account_acceptance", path)
acceptance = importlib.util.module_from_spec(spec)
spec.loader.exec_module(acceptance)


class HostedJoinTest(unittest.TestCase):
    def verify(self, platform, exit_code, results):
        error = subprocess.CalledProcessError(exit_code, ["account-driver", "join-call"])
        with unittest.mock.patch.object(sys, "platform", platform), \
                unittest.mock.patch.object(acceptance, "drive", side_effect=error) as drive, \
                unittest.mock.patch.object(acceptance, "wait_for",
                                           side_effect=lambda condition, message, timeout: acceptance.check(condition(), message)):
            try:
                acceptance.verify_hosted_capture(1, "https://fixture.invalid", results, {"audio": True, "video": False})
            finally:
                drive.assert_called_once_with(1, "join-call", "https://fixture.invalid")

    def test_unacknowledged_press_checks_the_actual_page_and_both_devices(self):
        self.verify("darwin", 75, {"page": "loaded", "audio": "live", "video": "NotFoundError"})
        with self.assertRaisesRegex(RuntimeError, "did not report"):
            self.verify("darwin", 75, {"page": "loaded"})
        with self.assertRaisesRegex(RuntimeError, "audio"):
            self.verify("darwin", 75, {"page": "loaded", "audio": "NotFoundError", "video": "NotFoundError"})

    def test_other_driver_failures_are_reported(self):
        for platform, code in (("darwin", 1), ("win32", 75)):
            with self.subTest(platform=platform, code=code), self.assertRaises(subprocess.CalledProcessError):
                self.verify(platform, code, {})


if __name__ == "__main__":
    unittest.main()
