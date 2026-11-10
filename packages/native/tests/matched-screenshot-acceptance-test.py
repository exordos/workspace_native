"""The matched-screenshot acceptance harness must be mechanical: compare-crops
pair semantics, explicit tolerance bands, and honest NOT_RUN verdicts."""
import hashlib
import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

try:
    from PIL import Image
except ImportError:  # pragma: no cover
    Image = None

REPO = Path(__file__).resolve().parents[1]
SCRIPT = Path(__import__('os').environ.get('NATIVE_SCREENSHOT_ACCEPTANCE_SCRIPT_UNDER_TEST',
    REPO / 'scripts/matched-screenshot-acceptance.py'))
MANIFEST = Path(__file__).resolve().parent / 'matched-screenshots.json'
BASELINE_DIR = Path(__file__).resolve().parent / 'screenshot-baselines'
COMPARE_CROPS_TOOL = Path(__import__('os').path.expanduser(__import__('os').environ.get('CASSI_COMPARE_CROPS_TOOL', '')))


def load_harness():
    spec = importlib.util.spec_from_file_location('matched_screenshot_acceptance', SCRIPT)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def make_png(path, size, color):
    Image.new('RGB', size, color).save(path)
    return Path(path)


def manifest_document(*screens):
    return {'version': 1, 'screens': list(screens)}


def screen(screen_id, **overrides):
    entry = {'id': screen_id, 'title': screen_id, 'binary': 'native-window-test',
        'case': 'realQuickWindowSignsInNavigatesAndSends',
        'capture': {'env': 'CASSI_NATIVE_TEST_SCREENSHOT'},
        'requires_identical_pixels': True, 'max_mean_abs_channel_difference': None,
        'tolerance_reason': None, 'reference': None}
    entry.update(overrides)
    return entry


@unittest.skipIf(Image is None, 'Pillow is required for screenshot comparisons')
class HarnessTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not SCRIPT.is_file():
            raise RuntimeError(f'{SCRIPT} is missing')  # RED: the harness must exist
        cls.harness = load_harness()

    def write_manifest(self, directory, *screens):
        import json
        path = Path(directory) / 'manifest.json'
        path.write_text(json.dumps(manifest_document(*screens)))
        return path

    # --- comparison maths (compare-crops pair semantics) ---

    def test_identical_images_pass_with_zero_difference(self):
        with tempfile.TemporaryDirectory() as directory:
            left = make_png(Path(directory) / 'a.png', (8, 6), (10, 20, 30))
            right = make_png(Path(directory) / 'b.png', (8, 6), (10, 20, 30))
            result = self.harness.compare_pair(left, right)
            self.assertFalse(result['rescaled'])
            self.assertTrue(result['sameDimensions'])
            self.assertTrue(result['identicalPixels'])
            self.assertEqual([round(value) for value in result['meanAbsoluteChannelDifference']], [0, 0, 0])
            for image in result['images']:
                self.assertEqual(image['originalSha256'], hashlib.sha256(left.read_bytes()).hexdigest())
                self.assertEqual(image['originalSize'], [8, 6])

    def test_uniform_channel_shift_reports_exact_mean_difference(self):
        with tempfile.TemporaryDirectory() as directory:
            left = make_png(Path(directory) / 'a.png', (8, 6), (10, 20, 30))
            right = make_png(Path(directory) / 'b.png', (8, 6), (30, 20, 30))
            result = self.harness.compare_pair(left, right)
            self.assertFalse(result['identicalPixels'])
            self.assertAlmostEqual(result['meanAbsoluteChannelDifference'][0], 20.0)
            self.assertAlmostEqual(result['meanAbsoluteChannelDifference'][1], 0.0)
            self.assertEqual(result['changedPixels'], 48)

    def test_changed_pixel_count_ignores_identical_pixels(self):
        with tempfile.TemporaryDirectory() as directory:
            left = Image.new('RGB', (10, 10), (10, 20, 30))
            right = Image.new('RGB', (10, 10), (10, 20, 30))
            right.putpixel((0, 0), (10, 20, 31))
            right.putpixel((9, 9), (11, 20, 30))
            right_path = Path(directory) / 'b.png'
            right.save(right_path)
            left_path = Path(directory) / 'a.png'
            left.save(left_path)
            result = self.harness.compare_pair(left_path, right_path)
            self.assertEqual(result['changedPixels'], 2)

    def test_changed_pixel_cap_fails_localized_regressions_below_the_mean_band(self):
        with tempfile.TemporaryDirectory() as directory:
            approved = Image.new('RGB', (1000, 1000), (10, 20, 30))
            drifted = Image.new('RGB', (1000, 1000), (10, 20, 30))
            for x in range(1000):  # one full row: mean shift ~0.02* but 1000 pixels changed
                drifted.putpixel((x, 500), (10, 60, 30))
            approved_path = Path(directory) / 'a.png'
            drifted_path = Path(directory) / 'b.png'
            approved.save(approved_path)
            drifted.save(drifted_path)
            comparison = self.harness.compare_pair(approved_path, drifted_path)
            self.assertLess(max(comparison['meanAbsoluteChannelDifference']), 0.1)
            self.assertEqual(comparison['changedPixels'], 1000)
            entry = screen('banded', requires_identical_pixels=False,
                max_mean_abs_channel_difference=0.1, tolerance_reason='documented host band',
                max_changed_pixels=500)
            result = self.screen_result(directory, entry, approved_path, drifted_path)
            self.assertEqual(result['baseline']['verdict'], 'FAIL')
            self.assertIn('changed pixels', result['baseline']['reason'])

    def test_dimension_mismatch_is_retained_and_never_rescaled(self):
        with tempfile.TemporaryDirectory() as directory:
            left = make_png(Path(directory) / 'a.png', (8, 6), (10, 20, 30))
            right = make_png(Path(directory) / 'b.png', (8, 5), (10, 20, 30))
            result = self.harness.compare_pair(left, right)
            self.assertFalse(result['sameDimensions'])
            self.assertIsNone(result['meanAbsoluteChannelDifference'])
            self.assertIn('comparisonNote', result)

    def test_documented_crop_is_applied_before_comparison(self):
        with tempfile.TemporaryDirectory() as directory:
            left = make_png(Path(directory) / 'a.png', (10, 10), (200, 0, 0))
            right = Image.new('RGB', (10, 10), (200, 0, 0))
            for x in range(10):  # a border that only the crop excludes
                for y in (0, 9):
                    right.putpixel((x, y), (0, 0, 255))
            right_path = Path(directory) / 'b.png'
            right.save(right_path)
            strict = self.harness.compare_pair(left, right_path)
            self.assertFalse(strict['identicalPixels'])
            interior = (0, 1, 10, 9)
            cropped = self.harness.compare_pair(left, right_path, interior, interior)
            self.assertTrue(cropped['identicalPixels'])
            self.assertEqual(cropped['images'][1]['size'], [10, 8])

    def test_crop_beyond_bounds_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            left = make_png(Path(directory) / 'a.png', (8, 6), (1, 2, 3))
            right = make_png(Path(directory) / 'b.png', (8, 6), (1, 2, 3))
            with self.assertRaises(ValueError):
                self.harness.compare_pair(left, right, None, (0, 0, 9, 6))

    # --- verdict rules ---

    def screen_result(self, temporary, entry, baseline=None, capture=None, reference=None):
        import shutil
        capture_dir = Path(temporary) / 'captures'
        capture_dir.mkdir(exist_ok=True)
        baseline_dir = Path(temporary) / 'baselines'
        baseline_dir.mkdir(exist_ok=True)
        reference_dir = Path(temporary) / 'references'
        reference_dir.mkdir(exist_ok=True)
        for value, directory, name in ((capture, capture_dir, f"{entry['id']}.png"),
                (baseline, baseline_dir, f"{entry['id']}.png"), (reference, reference_dir, entry.get('reference'))):
            if value is not None:
                shutil.copyfile(value, directory / name)
        return self.harness.check_screen(entry, capture_dir, baseline_dir, reference_dir)

    def test_missing_baseline_is_not_run_and_missing_capture_fails(self):
        with tempfile.TemporaryDirectory() as directory:
            entry = screen('chat-light')
            result = self.screen_result(directory, entry)
            self.assertEqual(result['baseline']['verdict'], 'NOT_RUN')
            self.assertEqual(result['baseline']['reason'], 'no approved baseline')
            self.assertEqual(result['capture'], 'MISSING')
            self.assertEqual(result['verdict'], 'FAIL')
            self.assertEqual(result['reason'], 'live capture missing')

    def test_identical_requirement_and_band_verdicts(self):
        with tempfile.TemporaryDirectory() as directory:
            approved = make_png(Path(directory) / 'approved.png', (8, 6), (10, 20, 30))
            same = make_png(Path(directory) / 'same.png', (8, 6), (10, 20, 30))
            shifted = make_png(Path(directory) / 'shifted.png', (8, 6), (12, 20, 30))
            strict = screen('strict')
            self.assertEqual(self.screen_result(directory, strict, approved, same)['baseline']['verdict'], 'PASS')
            self.assertEqual(self.screen_result(directory, strict, approved, shifted)['baseline']['verdict'], 'FAIL')
            banded = screen('banded', requires_identical_pixels=False,
                max_mean_abs_channel_difference=5.0, tolerance_reason='documented host band')
            self.assertEqual(self.screen_result(directory, banded, approved, shifted)['baseline']['verdict'], 'PASS')
            wide = make_png(Path(directory) / 'wide.png', (8, 6), (40, 20, 30))
            self.assertEqual(self.screen_result(directory, banded, approved, wide)['baseline']['verdict'], 'FAIL')

    def test_missing_reference_is_not_run_and_present_reference_is_compared(self):
        with tempfile.TemporaryDirectory() as directory:
            approved = make_png(Path(directory) / 'approved.png', (8, 6), (10, 20, 30))
            entry = screen('chat-light', reference='chat-light.png', reference_tolerance=1.0,
                reference_tolerance_reason='cross-renderer band measured from the matched pair')
            result = self.screen_result(directory, entry, approved, approved)
            self.assertEqual(result['reference']['verdict'], 'NOT_RUN')
            self.assertEqual(result['reference']['reason'], 'no approved source capture')
            result = self.screen_result(directory, entry, approved, approved, approved)
            self.assertEqual(result['reference']['verdict'], 'PASS')
            self.assertEqual(result['verdict'], 'PASS')

    def test_summary_counts_and_overall_verdict(self):
        results = [{'screen': 'a', 'verdict': 'PASS'}, {'screen': 'b', 'verdict': 'NOT_RUN'},
            {'screen': 'c', 'verdict': 'FAIL'}]
        summary = self.harness.build_summary(results)
        self.assertEqual(summary['verdict'], 'FAIL')
        self.assertEqual(summary['counts'], {'PASS': 1, 'FAIL': 1, 'NOT_RUN': 1})

    # --- manifest discipline ---

    def test_manifest_requires_explicit_tolerance_and_unique_ids(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaises(self.harness.ManifestError):
                self.harness.load_manifest(self.write_manifest(directory, screen('a'), screen('a')))
            loose = screen('loose', requires_identical_pixels=False,
                max_mean_abs_channel_difference=None, tolerance_reason=None)
            with self.assertRaises(self.harness.ManifestError):
                self.harness.load_manifest(self.write_manifest(directory, loose))
            band_without_reason = screen('band', requires_identical_pixels=False,
                max_mean_abs_channel_difference=3.0, tolerance_reason=None)
            with self.assertRaises(self.harness.ManifestError):
                self.harness.load_manifest(self.write_manifest(directory, band_without_reason))

    def test_manifest_capture_spec_must_name_env_hook(self):
        with tempfile.TemporaryDirectory() as directory:
            broken = screen('broken', capture={'file': 'x.png'})
            with self.assertRaises(self.harness.ManifestError):
                self.harness.load_manifest(self.write_manifest(directory, broken))

    def test_changed_pixel_cap_must_be_positive_when_present(self):
        with tempfile.TemporaryDirectory() as directory:
            entry = screen('capped', requires_identical_pixels=False,
                max_mean_abs_channel_difference=1.0, tolerance_reason='documented band',
                max_changed_pixels=0)
            with self.assertRaises(self.harness.ManifestError):
                self.harness.load_manifest(self.write_manifest(directory, entry))

    def test_shipped_manifest_validates_and_screens_are_registered(self):
        screens = self.harness.load_manifest(MANIFEST)
        self.assertGreaterEqual(len(screens), 14,
            'every existing capture seam must be a screen')
        self.assertEqual(len({entry['id'] for entry in screens}), len(screens))

    def test_committed_baselines_cover_the_shipped_manifest(self):
        if not (BASELINE_DIR / 'baselines.json').is_file():
            self.skipTest('no approved baselines in this checkout yet')
        import json
        screens = self.harness.load_manifest(MANIFEST)
        approved = json.loads((BASELINE_DIR / 'baselines.json').read_text())
        for entry in screens:
            approved_entry = approved['screens'].get(entry['id'])
            if approved_entry is None:
                self.skipTest(f'{entry["id"]} has no approved baseline yet')
            self.assertTrue((BASELINE_DIR / f"{entry['id']}.png").is_file())
            digest = hashlib.sha256((BASELINE_DIR / f"{entry['id']}.png").read_bytes()).hexdigest()
            self.assertEqual(digest, approved_entry['sha256'], f'{entry["id"]} baseline drifted')

    # --- capture planning (no Qt needed) ---

    def test_capture_plan_groups_screens_by_case_and_sets_hooks(self):
        screens = self.harness.load_manifest(MANIFEST)
        plan = self.harness.capture_plan(screens, Path('/tmp/build'), Path('/tmp/out'))
        by_case = {step['case']: step for step in plan}
        chat = by_case['realQuickWindowSignsInNavigatesAndSends']
        self.assertEqual(chat['binary'], '/tmp/build/native-window-test')
        hooks = {key for key in chat['env'] if key.startswith('CASSI_NATIVE_TEST_')}
        self.assertIn('CASSI_NATIVE_TEST_SCREENSHOT', hooks)
        self.assertIn('CASSI_NATIVE_TEST_MENU_DARK_SCREENSHOT', hooks)
        self.assertEqual(chat['env']['CASSI_NATIVE_TEST_DATE_FIXTURE'], '1')
        self.assertEqual(chat['env']['CASSI_NATIVE_TEST_SCREENSHOT'], '/tmp/out/chat-light.png')
        aggregate = by_case['aggregateStreamUsesSourceGroupsAndTopicChoices']
        self.assertEqual(aggregate['env']['CASSI_NATIVE_TEST_AGGREGATE_CAPTURE_DIR'], '/tmp/out')
        self.assertIn('native-aggregate-v15-ru-dark.png', aggregate['produces'])

    # --- whole-check run ---

    def run_check(self, temporary, manifest_path, expect):
        code = self.harness.main(['--manifest', str(manifest_path), '--baseline-dir',
            str(Path(temporary) / 'baselines'), '--reference-dir', str(Path(temporary) / 'references'),
            '--capture-dir', str(Path(temporary) / 'captures'), '--action', 'check',
            '--reuse-captures', '--json-out', str(Path(temporary) / 'report.json')])
        self.assertEqual(code, expect)
        import json
        return json.loads((Path(temporary) / 'report.json').read_text())

    def test_main_check_exits_zero_when_green_and_three_when_a_screen_drifts(self):
        with tempfile.TemporaryDirectory() as directory:
            approved = make_png(Path(directory) / 'approved.png', (8, 6), (10, 20, 30))
            entry = screen('chat-light')
            manifest_path = self.write_manifest(directory, entry)
            self.screen_result(directory, entry, approved, approved)  # creates the three dirs + capture
            report = self.run_check(directory, manifest_path, 0)
            self.assertEqual(report['verdict'], 'PASS')
            make_png(Path(directory) / 'captures' / 'chat-light.png', (8, 6), (77, 20, 30))
            report = self.run_check(directory, manifest_path, 3)
            self.assertEqual(report['screens'][0]['verdict'], 'FAIL')

    @unittest.skipUnless(COMPARE_CROPS_TOOL.is_file(), 'set CASSI_COMPARE_CROPS_TOOL to compare-crops.py')
    def test_numbers_match_the_toolkit_compare_crops_pair_mode(self):
        import json
        with tempfile.TemporaryDirectory() as directory:
            left = make_png(Path(directory) / 'a.png', (8, 6), (10, 20, 30))
            right = make_png(Path(directory) / 'b.png', (8, 6), (25, 20, 40))
            mine = self.harness.compare_pair(left, right)
            output = Path(directory) / 'toolkit'
            subprocess.run([sys.executable, str(COMPARE_CROPS_TOOL), str(left), str(right),
                '--output', str(output)], check=True, capture_output=True, timeout=60)
            theirs = json.loads((output / 'comparison.json').read_text())
            self.assertEqual([round(value, 6) for value in theirs['meanAbsoluteChannelDifference']],
                [round(value, 6) for value in mine['meanAbsoluteChannelDifference']])
            self.assertEqual(theirs['identicalPixels'], mine['identicalPixels'])
            self.assertEqual(theirs['rescaled'], mine['rescaled'])


if __name__ == '__main__':
    unittest.main()
