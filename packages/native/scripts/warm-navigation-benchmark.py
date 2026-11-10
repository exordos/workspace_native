#!/usr/bin/env python3
"""Per-release warm-navigation and aggregate performance baselines (issue #13).

Runs the documented ``CASSI_NATIVE_TEST_PERF_METRICS`` capture seam in
``native-window-test``, samples the test process RSS externally from
``/proc/<pid>/status``, and compares the capture against the registry gates:
exact GET-request counters (the warm-cache contract: 4 topic selections reduce
to 2 topic-history GETs, 2 stream selections to 1 aggregate-history GET and at
most 1 topic-list GET), a cache-hit minimum, and timing/RSS tolerance bands
whose reasons name the fluctuation source, per the issue #16 rule. Verdicts and
exit codes match scripts/matched-screenshot-acceptance.py: 0 all gates pass,
3 a gate failed, 1 a harness or registry error; a scenario without an approved
baseline is NOT_RUN, never a fake PASS. See
docs/NATIVE_DESKTOP_PERF_BASELINES.md.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import threading
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
DEFAULT_REGISTRY = REPO / 'tests/warm-navigation-benchmark.json'
DEFAULT_BASELINE_DIR = REPO / 'tests/perf-baselines'
RSS_SAMPLE_INTERVAL_S = 0.02


class HarnessError(Exception):
    """Infrastructure problem: bad invocation, missing tooling or build."""


class RegistryError(Exception):
    """The scenario registry or a measurement document is not a valid contract."""


def _positive_number(value):
    return isinstance(value, (int, float)) and not isinstance(value, bool) and value > 0


def load_registry(path):
    path = Path(path)
    try:
        document = json.loads(path.read_text())
    except FileNotFoundError as error:
        raise RegistryError(f'registry not found: {path}') from error
    except json.JSONDecodeError as error:
        raise RegistryError(f'registry is not valid JSON: {path}: {error}') from error
    scenarios = document.get('scenarios') if isinstance(document, dict) else None
    if not isinstance(scenarios, list) or not scenarios:
        raise RegistryError(f'registry must hold a non-empty "scenarios" list: {path}')
    seen = set()
    for entry in scenarios:
        scenario_id = entry.get('id')
        if not isinstance(scenario_id, str) or not scenario_id:
            raise RegistryError(f'every scenario needs a non-empty id: {entry}')
        if scenario_id in seen:
            raise RegistryError(f'duplicate scenario id: {scenario_id}')
        seen.add(scenario_id)
        for key in ('binary', 'case'):
            if not isinstance(entry.get(key), str) or not entry[key]:
                raise RegistryError(f'scenario {scenario_id} needs a {key}')
        metrics_env = entry.get('metrics_env')
        if not isinstance(metrics_env, str) or not metrics_env.startswith('CASSI_NATIVE_TEST_'):
            raise RegistryError(f'scenario {scenario_id} metrics_env must be a CASSI_NATIVE_TEST_* hook')
        requests = entry.get('requests')
        if not isinstance(requests, dict) or not requests:
            raise RegistryError(f'scenario {scenario_id} needs exact request gates')
        for family, gate in requests.items():
            if not isinstance(gate, int) or isinstance(gate, bool) or gate < 0:
                raise RegistryError(f'scenario {scenario_id} request gate {family} must be a non-negative integer')
        minimum_hits = entry.get('min_cache_hits')
        if not isinstance(minimum_hits, int) or isinstance(minimum_hits, bool) or minimum_hits < 1:
            raise RegistryError(f'scenario {scenario_id} min_cache_hits must be a positive integer')
        phases = entry.get('phases')
        if not isinstance(phases, list) or not phases:
            raise RegistryError(f'scenario {scenario_id} needs phase bands')
        for phase in phases:
            if not isinstance(phase, dict) or not isinstance(phase.get('name'), str) or not phase['name']:
                raise RegistryError(f'scenario {scenario_id} phase needs a name')
            if not _positive_number(phase.get('max_ms')):
                raise RegistryError(f'scenario {scenario_id} phase {phase["name"]} needs a positive max_ms')
            if not (isinstance(phase.get('tolerance_reason'), str) and phase['tolerance_reason'].strip()):
                raise RegistryError(f'scenario {scenario_id} phase {phase["name"]} band needs a tolerance_reason')
        if not _positive_number(entry.get('max_total_ms')):
            raise RegistryError(f'scenario {scenario_id} needs a positive max_total_ms')
        if not (isinstance(entry.get('total_tolerance_reason'), str) and entry['total_tolerance_reason'].strip()):
            raise RegistryError(f'scenario {scenario_id} total band needs a total_tolerance_reason')
        if not _positive_number(entry.get('max_peak_rss_kib')):
            raise RegistryError(f'scenario {scenario_id} needs a positive max_peak_rss_kib')
        if not (isinstance(entry.get('rss_tolerance_reason'), str) and entry['rss_tolerance_reason'].strip()):
            raise RegistryError(f'scenario {scenario_id} RSS band needs a rss_tolerance_reason')
        wall_gate = entry.get('max_case_wall_s')
        if wall_gate is not None and not _positive_number(wall_gate):
            raise RegistryError(f'scenario {scenario_id} max_case_wall_s must be positive when present')
    return scenarios


def load_metrics_from_text(text):
    try:
        document = json.loads(text)
    except json.JSONDecodeError as error:
        raise RegistryError(f'measurement document is not valid JSON: {error}') from error
    return _validate_metrics(document)


def _validate_metrics(document):
    if not isinstance(document, dict):
        raise RegistryError('measurement document must be a JSON object')
    requests = document.get('requests')
    if not isinstance(requests, dict) or not requests:
        raise RegistryError('measurement document needs request counters')
    for family, value in requests.items():
        if not isinstance(value, int) or isinstance(value, bool) or value < 0:
            raise RegistryError(f'measurement counter {family} must be a non-negative integer')
    hits = document.get('cacheHits')
    if not isinstance(hits, int) or isinstance(hits, bool) or hits < 0:
        raise RegistryError('measurement document needs a non-negative cacheHits counter')
    phases = document.get('phases')
    if not isinstance(phases, list) or not phases:
        raise RegistryError('measurement document needs phase timings')
    for phase in phases:
        if not isinstance(phase, dict) or not isinstance(phase.get('name'), str) or not phase['name']:
            raise RegistryError('every phase timing needs a name')
        if not isinstance(phase.get('ms'), (int, float)) or isinstance(phase.get('ms'), bool):
            raise RegistryError(f'phase {phase["name"]} needs a numeric ms')
    if not isinstance(document.get('totalMs'), (int, float)) or isinstance(document.get('totalMs'), bool):
        raise RegistryError('measurement document needs a numeric totalMs')
    return document


def load_metrics(path):
    path = Path(path)
    try:
        return load_metrics_from_text(path.read_text())
    except FileNotFoundError:
        raise


def parse_proc_status(text):
    result = {}
    for line in text.splitlines():
        if line.startswith('VmHWM:'):
            result['VmHWMKiB'] = int(line.split()[1])
        elif line.startswith('VmRSS:'):
            result['VmRSSKiB'] = int(line.split()[1])
    return result


def capture_plan(scenarios, build_dir, capture_dir):
    build_dir, capture_dir = Path(build_dir), Path(capture_dir)
    steps = []
    for entry in scenarios:
        steps.append({'binary': str(build_dir / entry['binary']), 'case': entry['case'],
            'args': [entry['case']],
            'env': {entry['metrics_env']: str(capture_dir / f"{entry['id']}.raw.json")},
            'id': entry['id'], 'produces': f"{entry['id']}.json"})
    return steps


def _sample_rss(stop_event, pid, samples):
    status_path = Path(f'/proc/{pid}/status')
    while not stop_event.is_set():
        try:
            parsed = parse_proc_status(status_path.read_text())
        except (FileNotFoundError, ValueError, IndexError):
            return
        samples['peakRssKiB'] = max(samples.get('peakRssKiB', 0), parsed.get('VmHWMKiB', 0),
            parsed.get('VmRSSKiB', 0))
        samples['maxVmRssSampleKiB'] = max(samples.get('maxVmRssSampleKiB', 0), parsed.get('VmRSSKiB', 0))
        stop_event.wait(RSS_SAMPLE_INTERVAL_S)


def run_capture(plan, timeout):
    failures = []
    for step in plan:
        if not Path(step['binary']).is_file():
            raise HarnessError(f'test binary not found: {step["binary"]}')
        environment = dict(os.environ)
        environment.update(step['env'])
        started = time.monotonic()
        process = subprocess.Popen([step['binary'], *step['args']], env=environment,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        stop_event = threading.Event()
        samples = {}
        sampler = threading.Thread(target=_sample_rss, args=(stop_event, process.pid, samples), daemon=True)
        sampler.start()
        try:
            stdout, stderr = process.communicate(timeout=timeout)
        except subprocess.TimeoutExpired:
            process.kill()
            stop_event.set()
            sampler.join(timeout=1)
            failures.append(f"{step['case']}: timed out after {timeout}s")
            continue
        stop_event.set()
        sampler.join(timeout=1)
        elapsed = time.monotonic() - started
        status = 'ok' if process.returncode == 0 else f'exit {process.returncode}'
        print(f"  capture run {step['case']}: {status} in {elapsed:.0f}s", file=sys.stderr)
        if process.returncode != 0:
            failures.append(f"{step['case']}:\n{(stdout + stderr)[-800:]}")
            continue
        raw_path = Path(step['env'][next(iter(step['env']))])
        try:
            document = load_metrics(raw_path)
        except (FileNotFoundError, RegistryError) as error:
            failures.append(f"{step['case']}: no usable measurement document ({error})")
            continue
        document['process'] = {'peakRssKiB': samples.get('peakRssKiB'),
            'maxVmRssSampleKiB': samples.get('maxVmRssSampleKiB')}
        document['caseWallSeconds'] = round(elapsed, 3)
        (raw_path.parent / step['produces']).write_text(json.dumps(document, indent=2) + '\n')
    return failures


def check_scenario(entry, capture_dir, baseline_dir):
    capture_dir, baseline_dir = Path(capture_dir), Path(baseline_dir)
    capture_path = capture_dir / f"{entry['id']}.json"
    baseline_path = baseline_dir / f"{entry['id']}.json"
    result = {'scenario': entry['id'], 'title': entry.get('title', entry['id']),
        'capture': 'MISSING', 'baseline': 'missing', 'gates': {}, 'delta': None,
        'verdict': 'FAIL', 'reason': 'live capture missing'}
    if not capture_path.is_file():
        return result
    document = load_metrics(capture_path)
    result['capture'] = 'OK'
    if baseline_path.is_file():
        result['baseline'] = 'approved'
    gates = {}
    failures = []
    requests = {}
    for family, gate in entry['requests'].items():
        actual = document['requests'].get(family)
        requests[family] = {'gate': gate, 'measured': actual,
            'ok': actual is not None and actual <= gate}
        if not requests[family]['ok']:
            detail = 'missing' if actual is None else f'{actual} exceeds the gate of {gate}'
            failures.append(f'{family} request count {detail}')
    gates['requests'] = 'PASS' if not failures else 'FAIL'
    hits_ok = document['cacheHits'] >= entry['min_cache_hits']
    gates['cacheHits'] = 'PASS' if hits_ok else 'FAIL'
    if not hits_ok:
        failures.append(f"cache hits {document['cacheHits']} below the minimum of {entry['min_cache_hits']}")
    measured_phases = {phase['name']: phase['ms'] for phase in document['phases']}
    phase_gates = {}
    for band in entry['phases']:
        actual = measured_phases.get(band['name'])
        ok = actual is not None and actual <= band['max_ms']
        phase_gates[band['name']] = {'max_ms': band['max_ms'], 'measured': actual, 'ok': ok}
        if actual is None:
            failures.append(f"phase {band['name']} is missing from the capture")
        elif not ok:
            failures.append(f"phase {band['name']} took {actual:.1f} ms above its band of {band['max_ms']} ms")
    gates['phases'] = 'PASS' if all(gate['ok'] for gate in phase_gates.values()) and len(phase_gates) == len(entry['phases']) else 'FAIL'
    total_ok = document['totalMs'] <= entry['max_total_ms']
    gates['totalMs'] = 'PASS' if total_ok else 'FAIL'
    if not total_ok:
        failures.append(f"total navigation time {document['totalMs']:.1f} ms exceeds its band of {entry['max_total_ms']} ms")
    process_stats = document.get('process') or {}
    peak_rss = process_stats.get('peakRssKiB')
    rss_ok = peak_rss is not None and peak_rss <= entry['max_peak_rss_kib']
    gates['peakRssKiB'] = 'PASS' if rss_ok else 'FAIL'
    if peak_rss is None:
        failures.append('peak RSS was not sampled')
    elif not rss_ok:
        failures.append(f'peak RSS {peak_rss} KiB exceeds its band of {entry["max_peak_rss_kib"]} KiB')
    wall_gate = entry.get('max_case_wall_s')
    if wall_gate is not None:
        wall = document.get('caseWallSeconds')
        wall_ok = wall is not None and wall <= wall_gate
        gates['caseWallSeconds'] = 'PASS' if wall_ok else 'FAIL'
        if not wall_ok:
            failures.append(f'case wall time {wall} s exceeds its band of {wall_gate} s')
    result['gates'] = gates
    result['measurements'] = {'requests': document['requests'], 'cacheHits': document['cacheHits'],
        'phases': measured_phases, 'totalMs': document['totalMs'], 'peakRssKiB': peak_rss,
        'caseWallSeconds': document.get('caseWallSeconds')}
    if baseline_path.is_file():
        baseline = json.loads(baseline_path.read_text())
        baseline_phases = {phase['name']: phase['ms'] for phase in baseline.get('phases', [])}
        result['delta'] = {'totalMs': round(document['totalMs'] - baseline.get('totalMs', 0.0), 3),
            'cacheHits': document['cacheHits'] - baseline.get('cacheHits', 0),
            'phasesMs': {name: round(actual - baseline_phases.get(name, actual), 3)
                for name, actual in measured_phases.items()}}
    if failures:
        result['verdict'], result['reason'] = 'FAIL', '; '.join(failures)
    elif not baseline_path.is_file():
        result['verdict'], result['reason'] = 'NOT_RUN', 'no approved baseline'
    else:
        result['verdict'], result['reason'] = 'PASS', None
    return result


def build_summary(results):
    counts = {'PASS': 0, 'FAIL': 0, 'NOT_RUN': 0}
    for result in results:
        counts[result['verdict']] = counts.get(result['verdict'], 0) + 1
    return {'verdict': 'FAIL' if counts['FAIL'] else 'PASS', 'counts': counts, 'scenarios': results}


def _record():
    try:
        return subprocess.run(['git', '-C', str(REPO), 'rev-parse', 'HEAD'],
            capture_output=True, text=True, timeout=10).stdout.strip() or 'unknown'
    except OSError:
        return 'unknown'


def write_baselines(scenarios, capture_dir, baseline_dir):
    baseline_dir = Path(baseline_dir)
    baseline_dir.mkdir(parents=True, exist_ok=True)
    approved = []
    for entry in scenarios:
        source = Path(capture_dir) / f"{entry['id']}.json"
        if not source.is_file():
            raise HarnessError(f'cannot approve a baseline without a capture: {entry["id"]}')
        document = json.loads(source.read_text())
        document['generatedUtc'] = time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())
        document['repositoryRevision'] = _record()
        (baseline_dir / f"{entry['id']}.json").write_text(json.dumps(document, indent=2) + '\n')
        approved.append(entry['id'])
    return approved


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--registry', type=Path, default=DEFAULT_REGISTRY)
    parser.add_argument('--build', type=Path, help='directory containing native-window-test')
    parser.add_argument('--capture-dir', type=Path, required=True)
    parser.add_argument('--baseline-dir', type=Path, default=DEFAULT_BASELINE_DIR)
    parser.add_argument('--action', choices=('capture', 'baseline', 'check'), required=True)
    parser.add_argument('--reuse-captures', action='store_true',
        help='do not run the QtTest case; evaluate the captures already in --capture-dir')
    parser.add_argument('--only', action='append', default=[], help='limit to a scenario id (repeatable)')
    parser.add_argument('--timeout', type=int, default=600, help='seconds per QtTest case run')
    parser.add_argument('--json-out', type=Path)
    args = parser.parse_args(argv)

    try:
        scenarios = load_registry(args.registry)
        if args.only:
            unknown = set(args.only) - {entry['id'] for entry in scenarios}
            if unknown:
                raise HarnessError(f'unknown scenario ids: {", ".join(sorted(unknown))}')
            scenarios = [entry for entry in scenarios if entry['id'] in args.only]
        args.capture_dir.mkdir(parents=True, exist_ok=True)
        if not args.reuse_captures:
            if args.build is None:
                raise HarnessError('--build is required unless --reuse-captures is set')
            failures = run_capture(capture_plan(scenarios, args.build, args.capture_dir), args.timeout)
            if failures:
                print('capture runs failed:\n' + '\n'.join(failures), file=sys.stderr)
                return 1
        if args.action == 'capture':
            for entry in scenarios:
                print(f"{entry['id']}: captured {(args.capture_dir / f'{entry["id"]}.json').is_file()}")
            return 0
        if args.action == 'baseline':
            approved = write_baselines(scenarios, args.capture_dir, args.baseline_dir)
            print(f'approved {len(approved)} baselines into {args.baseline_dir}')
            return 0
        results = [check_scenario(entry, args.capture_dir, args.baseline_dir) for entry in scenarios]
        summary = build_summary(results)
        for result in results:
            extra = f' ({result["reason"]})' if result.get('reason') else ''
            print(f'{result["verdict"]:>7}  {result["scenario"]:<24} '
                f'baseline={result["baseline"]} {result["gates"]}{extra}')
        print('overall: ' + summary['verdict'] + ' ' +
            ' '.join(f'{key}={value}' for key, value in summary['counts'].items()))
        if args.json_out:
            args.json_out.parent.mkdir(parents=True, exist_ok=True)
            args.json_out.write_text(json.dumps(summary, indent=2) + '\n')
        return 3 if summary['verdict'] == 'FAIL' else 0
    except HarnessError as error:
        print(f'error: {error}', file=sys.stderr)
        return 1
    except RegistryError as error:
        print(f'registry error: {error}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
