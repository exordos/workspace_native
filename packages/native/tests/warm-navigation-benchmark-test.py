"""The warm-navigation performance benchmark must be mechanical: exact
request-count gates, tolerance bands with named reasons, honest NOT_RUN
verdicts, and per-release deltas against the approved baseline (issue #13).
See docs/NATIVE_DESKTOP_PERF_BASELINES.md."""
import importlib.util
import json
import os
from pathlib import Path
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[1]
SCRIPT = Path(os.environ.get('NATIVE_PERF_BENCHMARK_SCRIPT_UNDER_TEST',
    REPO / 'scripts/warm-navigation-benchmark.py'))
REGISTRY = Path(__file__).resolve().parent / 'warm-navigation-benchmark.json'
BASELINE_DIR = Path(__file__).resolve().parent / 'perf-baselines'


def load_harness():
    spec = importlib.util.spec_from_file_location('warm_navigation_benchmark', SCRIPT)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def scenario(scenario_id='warm-navigation', **overrides):
    entry = {
        'id': scenario_id,
        'title': 'warm navigation',
        'binary': 'native-window-test',
        'case': 'warmNavigationUsesVerifiedSavedPagesWithoutExtraHttpRequests',
        'metrics_env': 'CASSI_NATIVE_TEST_PERF_METRICS',
        'requests': {'topic-history': 2, 'aggregate-history': 1, 'topic-list': 1},
        'min_cache_hits': 4,
        'phases': [
            {'name': 'project-open', 'max_ms': 900, 'tolerance_reason': 'fixture band'},
            {'name': 'topic-select-3-warm', 'max_ms': 400, 'tolerance_reason': 'fixture band'},
        ],
        'max_total_ms': 9000,
        'total_tolerance_reason': 'fixture band',
        'max_peak_rss_kib': 900000,
        'rss_tolerance_reason': 'fixture band',
    }
    entry.update(overrides)
    return entry


def metrics(**overrides):
    document = {
        'version': 1,
        'case': 'warmNavigationUsesVerifiedSavedPagesWithoutExtraHttpRequests',
        'synthetic': True,
        'requests': {'topic-history': 2, 'aggregate-history': 1, 'topic-list': 1},
        'cacheHits': 4,
        'phases': [
            {'name': 'sign-in', 'ms': 120.0},
            {'name': 'project-open', 'ms': 310.0},
            {'name': 'stream-open-1-cold', 'ms': 250.0},
            {'name': 'topic-select-1-cold', 'ms': 300.0},
            {'name': 'topic-select-2-cold', 'ms': 280.0},
            {'name': 'topic-select-3-warm', 'ms': 90.0},
            {'name': 'stream-open-2-warm', 'ms': 70.0},
            {'name': 'topic-select-4-warm', 'ms': 60.0},
        ],
        'totalMs': 1500.0,
    }
    document.update(overrides)
    return document


def capture_document(**overrides):
    document = metrics()
    document['process'] = {'peakRssKiB': 250000, 'maxVmRssSampleKiB': 240000}
    document['caseWallSeconds'] = 12.5
    document.update(overrides)
    return document


class RegistryTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not SCRIPT.is_file():
            raise RuntimeError(f'{SCRIPT} is missing')  # RED: the harness must exist
        cls.harness = load_harness()

    def write_registry(self, directory, *scenarios):
        path = Path(directory) / 'registry.json'
        path.write_text(json.dumps({'version': 1, 'scenarios': list(scenarios)}))
        return path

    def test_committed_registry_loads_and_carries_the_warm_navigation_scenario(self):
        scenarios = self.harness.load_registry(REGISTRY)
        self.assertIn('warm-navigation', [entry['id'] for entry in scenarios])
        for entry in scenarios:
            self.assertEqual(entry['binary'], 'native-window-test')
            self.assertGreaterEqual(entry['min_cache_hits'], 1)
            for phase in entry['phases']:
                self.assertTrue(phase['tolerance_reason'].strip())

    def test_committed_baseline_dir_holds_a_baseline_per_scenario(self):
        for entry in self.harness.load_registry(REGISTRY):
            path = BASELINE_DIR / f"{entry['id']}.json"
            self.assertTrue(path.is_file(), f'missing approved baseline: {path}')
            json.loads(path.read_text())

    def test_missing_registry_is_a_registry_error(self):
        with self.assertRaises(self.harness.RegistryError):
            self.harness.load_registry(Path(tempfile.mkdtemp()) / 'absent.json')

    def test_registry_rejects_bad_entries(self):
        cases = [
            {'requests': {'topic-history': 'two'}},
            {'min_cache_hits': -1},
            {'metrics_env': 'NOT_A_CASSI_HOOK'},
            {'phases': [{'name': 'x', 'max_ms': 5}]},  # band without tolerance_reason
            {'max_total_ms': 5, 'total_tolerance_reason': ''},  # total band without reason
            {'max_peak_rss_kib': 5, 'rss_tolerance_reason': ''},  # RSS band without reason
            {'case': ''},
        ]
        for overrides in cases:
            with tempfile.TemporaryDirectory() as directory:
                with self.assertRaises(self.harness.RegistryError, msg=overrides):
                    self.harness.load_registry(self.write_registry(directory, scenario(**overrides)))

    def test_registry_rejects_duplicate_ids(self):
        with tempfile.TemporaryDirectory() as directory:
            path = self.write_registry(directory, scenario(), scenario())
            with self.assertRaises(self.harness.RegistryError):
                self.harness.load_registry(path)


class MetricsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.harness = load_harness()

    def test_valid_metrics_document_parses(self):
        parsed = self.harness.load_metrics_from_text(json.dumps(metrics()))
        self.assertEqual(parsed['requests']['topic-history'], 2)
        self.assertEqual(len(parsed['phases']), 8)

    def test_rejects_missing_or_malformed_fields(self):
        bad = [
            '{not json',
            json.dumps({'requests': {}}),  # no phases
            json.dumps({**metrics(), 'requests': {'topic-history': 'x'}}),
            json.dumps({**metrics(), 'cacheHits': -2}),
            json.dumps({**metrics(), 'phases': [{'name': 'a'}]}),  # no ms
        ]
        for text in bad:
            with self.assertRaises(self.harness.RegistryError):
                self.harness.load_metrics_from_text(text)

    def test_parse_proc_status_extracts_rss_fields(self):
        text = ('Name:\tnative-window-test\nVmHWM:\t  251000 kB\nVmSize:\t900000 kB\n'
                'VmRSS:\t  240500 kB\nThreads:\t9\n')
        parsed = self.harness.parse_proc_status(text)
        self.assertEqual(parsed['VmHWMKiB'], 251000)
        self.assertEqual(parsed['VmRSSKiB'], 240500)


class CapturePlanTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.harness = load_harness()

    def test_plan_wires_case_arguments_and_metrics_env(self):
        plan = self.harness.capture_plan([scenario()], Path('/tmp/build'), Path('/tmp/caps'))
        self.assertEqual(len(plan), 1)
        step = plan[0]
        self.assertEqual(step['binary'], '/tmp/build/native-window-test')
        self.assertEqual(step['args'], ['warmNavigationUsesVerifiedSavedPagesWithoutExtraHttpRequests'])
        self.assertEqual(step['env']['CASSI_NATIVE_TEST_PERF_METRICS'], '/tmp/caps/warm-navigation.raw.json')


class CheckTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.harness = load_harness()

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.capture_dir = self.root / 'captures'
        self.baseline_dir = self.root / 'baselines'
        self.capture_dir.mkdir()
        self.baseline_dir.mkdir()

    def stage(self, entry=None, capture=None, with_baseline=True):
        entry = entry or scenario()
        if capture is not None:
            (self.capture_dir / f"{entry['id']}.json").write_text(json.dumps(capture))
        if with_baseline:
            (self.baseline_dir / f"{entry['id']}.json").write_text(json.dumps(capture_document()))
        return entry

    def test_matching_capture_passes_and_reports_delta(self):
        entry = self.stage(capture=capture_document())
        result = self.harness.check_scenario(entry, self.capture_dir, self.baseline_dir)
        self.assertEqual(result['verdict'], 'PASS')
        self.assertEqual(result['gates']['requests'], 'PASS')
        # baseline totalMs 1500.0 versus capture 1500.0 -> zero delta
        self.assertEqual(result['delta']['totalMs'], 0.0)

    def test_extra_topic_history_get_fails(self):
        capture = capture_document(requests={'topic-history': 3, 'aggregate-history': 1, 'topic-list': 1})
        entry = self.stage(capture=capture)
        result = self.harness.check_scenario(entry, self.capture_dir, self.baseline_dir)
        self.assertEqual(result['verdict'], 'FAIL')
        self.assertIn('topic-history', result['reason'])

    def test_missing_request_family_in_capture_fails(self):
        capture = capture_document(requests={'topic-history': 2})
        entry = self.stage(capture=capture)
        result = self.harness.check_scenario(entry, self.capture_dir, self.baseline_dir)
        self.assertEqual(result['verdict'], 'FAIL')

    def test_cache_hits_below_minimum_fails(self):
        capture = capture_document(cacheHits=3)
        entry = self.stage(capture=capture)
        result = self.harness.check_scenario(entry, self.capture_dir, self.baseline_dir)
        self.assertEqual(result['verdict'], 'FAIL')
        self.assertIn('cache hits', result['reason'])

    def test_phase_above_band_fails(self):
        capture = capture_document()
        capture['phases'] = [dict(row) for row in capture['phases']]
        next(row for row in capture['phases'] if row['name'] == 'topic-select-3-warm')['ms'] = 450.0
        entry = self.stage(capture=capture)
        result = self.harness.check_scenario(entry, self.capture_dir, self.baseline_dir)
        self.assertEqual(result['verdict'], 'FAIL')
        self.assertIn('topic-select-3-warm', result['reason'])

    def test_registry_phase_missing_from_capture_fails(self):
        capture = capture_document()
        capture['phases'] = [row for row in capture['phases'] if row['name'] != 'project-open']
        entry = self.stage(capture=capture)
        result = self.harness.check_scenario(entry, self.capture_dir, self.baseline_dir)
        self.assertEqual(result['verdict'], 'FAIL')
        self.assertIn('project-open', result['reason'])

    def test_total_above_band_fails(self):
        capture = capture_document(totalMs=9500.0)
        entry = self.stage(capture=capture)
        result = self.harness.check_scenario(entry, self.capture_dir, self.baseline_dir)
        self.assertEqual(result['verdict'], 'FAIL')
        self.assertIn('total', result['reason'].lower())

    def test_peak_rss_above_band_fails(self):
        capture = capture_document(process={'peakRssKiB': 950000, 'maxVmRssSampleKiB': 940000})
        entry = self.stage(capture=capture)
        result = self.harness.check_scenario(entry, self.capture_dir, self.baseline_dir)
        self.assertEqual(result['verdict'], 'FAIL')
        self.assertIn('RSS', result['reason'])

    def test_missing_capture_fails(self):
        entry = self.stage(capture=None)
        result = self.harness.check_scenario(entry, self.capture_dir, self.baseline_dir)
        self.assertEqual(result['verdict'], 'FAIL')

    def test_missing_baseline_is_not_run_never_fake_pass(self):
        entry = self.stage(capture=capture_document(), with_baseline=False)
        result = self.harness.check_scenario(entry, self.capture_dir, self.baseline_dir)
        self.assertEqual(result['verdict'], 'NOT_RUN')
        self.assertNotEqual(result['verdict'], 'PASS')
        self.assertEqual(result['gates']['requests'], 'PASS')


class SummaryAndBaselineTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.harness = load_harness()

    def test_summary_counts_and_verdict(self):
        summary = self.harness.build_summary([{'verdict': 'PASS'}, {'verdict': 'NOT_RUN'}])
        self.assertEqual(summary['verdict'], 'PASS')
        self.assertEqual(summary['counts'], {'PASS': 1, 'FAIL': 0, 'NOT_RUN': 1})
        summary = self.harness.build_summary([{'verdict': 'PASS'}, {'verdict': 'FAIL'}])
        self.assertEqual(summary['verdict'], 'FAIL')

    def test_baseline_approval_round_trips_and_carries_provenance(self):
        with tempfile.TemporaryDirectory() as directory:
            capture_dir = Path(directory) / 'captures'
            baseline_dir = Path(directory) / 'baselines'
            capture_dir.mkdir()
            (capture_dir / 'warm-navigation.json').write_text(json.dumps(capture_document()))
            entry = scenario()
            self.harness.write_baselines([entry], capture_dir, baseline_dir)
            document = json.loads((baseline_dir / 'warm-navigation.json').read_text())
            self.assertIn('generatedUtc', document)
            self.assertIn('repositoryRevision', document)
            self.assertEqual(document['requests']['topic-history'], 2)


class MainExitCodeTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.harness = load_harness()

    def test_check_exit_codes_and_reuse_captures(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            capture_dir = root / 'captures'
            baseline_dir = root / 'baselines'
            capture_dir.mkdir()
            baseline_dir.mkdir()
            registry = root / 'registry.json'
            registry.write_text(json.dumps({'version': 1, 'scenarios': [scenario()]}))
            good = capture_document()
            (capture_dir / 'warm-navigation.json').write_text(json.dumps(good))
            (baseline_dir / 'warm-navigation.json').write_text(json.dumps(good))
            self.assertEqual(self.harness.main([
                '--registry', str(registry), '--capture-dir', str(capture_dir),
                '--baseline-dir', str(baseline_dir), '--action', 'check', '--reuse-captures',
            ]), 0)
            bad = capture_document(requests={'topic-history': 5, 'aggregate-history': 1, 'topic-list': 1})
            (capture_dir / 'warm-navigation.json').write_text(json.dumps(bad))
            self.assertEqual(self.harness.main([
                '--registry', str(registry), '--capture-dir', str(capture_dir),
                '--baseline-dir', str(baseline_dir), '--action', 'check', '--reuse-captures',
            ]), 3)

    def test_missing_binary_is_a_harness_error_exit_1(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            registry = root / 'registry.json'
            registry.write_text(json.dumps({'version': 1, 'scenarios': [scenario()]}))
            self.assertEqual(self.harness.main([
                '--registry', str(registry), '--build', str(root / 'nope'),
                '--capture-dir', str(root / 'caps'), '--baseline-dir', str(root / 'bl'),
                '--action', 'capture',
            ]), 1)


if __name__ == '__main__':
    unittest.main(verbosity=2)
