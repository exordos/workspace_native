#!/usr/bin/env python3
"""Per-screen matched-screenshot acceptance for the native client (issue #14).

Drives the documented ``CASSI_NATIVE_TEST_*_SCREENSHOT`` capture seams in
``native-window-test``, then compares every capture mechanically against the
approved native baseline and, where one exists, the approved source capture.
Comparison semantics follow the handoff toolkit ``compare-crops.py`` pair mode
verbatim: crops only, pixels are never rescaled, per-channel mean absolute
difference and an identical-pixels verdict, with the SHA-256 of every compared
file recorded in the report. See docs/NATIVE_DESKTOP_SCREENSHOT_ACCEPTANCE.md.
"""
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
DEFAULT_MANIFEST = REPO / 'tests/matched-screenshots.json'
DEFAULT_BASELINE_DIR = REPO / 'tests/screenshot-baselines'
DEFAULT_REFERENCE_DIR = REPO / 'tests/screenshot-references'
DATE_FIXTURE_ENV = 'CASSI_NATIVE_TEST_DATE_FIXTURE'


class HarnessError(Exception):
    """Infrastructure problem: bad invocation, missing tooling or build."""


class ManifestError(Exception):
    """The screen manifest does not carry an explicit, complete contract."""


def _positive_number(value):
    return isinstance(value, (int, float)) and not isinstance(value, bool) and value > 0


def load_manifest(path):
    path = Path(path)
    try:
        document = json.loads(path.read_text())
    except FileNotFoundError as error:
        raise ManifestError(f'manifest not found: {path}') from error
    except json.JSONDecodeError as error:
        raise ManifestError(f'manifest is not valid JSON: {path}: {error}') from error
    screens = document.get('screens') if isinstance(document, dict) else None
    if not isinstance(screens, list) or not screens:
        raise ManifestError(f'manifest must hold a non-empty "screens" list: {path}')
    seen = set()
    for entry in screens:
        screen_id = entry.get('id')
        if not isinstance(screen_id, str) or not screen_id:
            raise ManifestError(f'every screen needs a non-empty id: {entry}')
        if screen_id in seen:
            raise ManifestError(f'duplicate screen id: {screen_id}')
        seen.add(screen_id)
        for key in ('binary', 'case'):
            if not isinstance(entry.get(key), str) or not entry[key]:
                raise ManifestError(f'screen {screen_id} needs a {key}')
        capture = entry.get('capture')
        if not isinstance(capture, dict):
            raise ManifestError(f'screen {screen_id} needs a capture spec')
        env = capture.get('env')
        file_name = capture.get('file')
        dir_env = capture.get('dir_env')
        if env is not None and (not isinstance(env, str) or not env.startswith('CASSI_NATIVE_TEST_')):
            raise ManifestError(f'screen {screen_id} capture env must be a CASSI_NATIVE_TEST_* hook')
        if env is None:
            if not (isinstance(file_name, str) and isinstance(dir_env, str) and file_name and dir_env):
                raise ManifestError(f'screen {screen_id} capture needs env or dir_env+file')
        elif file_name is not None or dir_env is not None:
            raise ManifestError(f'screen {screen_id} capture combines env with dir_env/file')
        requires_identical = entry.get('requires_identical_pixels', False)
        band = entry.get('max_mean_abs_channel_difference')
        reason = entry.get('tolerance_reason')
        if requires_identical is True:
            if band is not None:
                raise ManifestError(f'screen {screen_id} cannot combine identical-pixels with a band')
        elif band is None:
            raise ManifestError(
                f'screen {screen_id} needs requires_identical_pixels or a documented band')
        else:
            if not _positive_number(band):
                raise ManifestError(f'screen {screen_id} band must be a positive number')
            if not (isinstance(reason, str) and reason.strip()):
                raise ManifestError(
                    f'screen {screen_id} band needs a tolerance_reason naming the fluctuating pixel source')
        cap = entry.get('max_changed_pixels')
        if cap is not None and (not isinstance(cap, int) or isinstance(cap, bool) or cap < 1):
            raise ManifestError(f'screen {screen_id} max_changed_pixels must be a positive integer')
        reference = entry.get('reference')
        if reference is not None:
            if not isinstance(reference, str) or not reference:
                raise ManifestError(f'screen {screen_id} reference must be null or a file name')
            if not _positive_number(entry.get('reference_tolerance')):
                raise ManifestError(f'screen {screen_id} reference needs a numeric reference_tolerance')
            reason = entry.get('reference_tolerance_reason')
            if not (isinstance(reason, str) and reason.strip()):
                raise ManifestError(f'screen {screen_id} reference needs a reference_tolerance_reason')
    return screens


def capture_filename(entry):
    file_name = entry['capture'].get('file')
    return file_name if file_name else f"{entry['id']}.png"


def compare_pair(source, native, source_rect=None, native_rect=None):
    """compare-crops.py pair-mode semantics; input pixels are never rescaled."""
    try:
        from PIL import Image, ImageChops, ImageStat
    except ImportError as error:  # pragma: no cover
        raise HarnessError(f'Pillow is required for screenshot comparison: {error}') from error

    def rectangle(value):
        parts = tuple(int(part) for part in value)
        if len(parts) != 4 or min(parts) < 0 or parts[2] <= parts[0] or parts[3] <= parts[1]:
            raise ValueError('Expected left,top,right,bottom with positive area')
        return parts

    result = {'rescaled': False, 'images': []}
    images = []
    for name, path, rect in (('source', Path(source), source_rect), ('native', Path(native), native_rect)):
        rect = rectangle(rect) if rect else None
        with Image.open(path) as opened:
            original = opened.convert('RGB')
            if rect and (rect[2] > opened.width or rect[3] > opened.height):
                raise ValueError('Crop exceeds original image bounds')
            image = original.crop(rect) if rect else original
            images.append(image)
            result['images'].append({'kind': name,
                'originalSha256': hashlib.sha256(path.read_bytes()).hexdigest(),
                'originalSize': [opened.width, opened.height], 'crop': list(rect) if rect else None,
                'size': [image.width, image.height]})
    result['sameDimensions'] = images[0].size == images[1].size
    if result['sameDimensions']:
        difference = ImageChops.difference(*images)
        result['meanAbsoluteChannelDifference'] = list(ImageStat.Stat(difference).mean)
        result['identicalPixels'] = difference.getbbox() is None
        red, green, blue = difference.split()
        max_channel = ImageChops.lighter(ImageChops.lighter(red, green), blue)
        result['changedPixels'] = sum(max_channel.histogram()[1:])
    else:
        result['meanAbsoluteChannelDifference'] = None
        result['identicalPixels'] = None
        result['changedPixels'] = None
        result['comparisonNote'] = 'Different dimensions retained; no forced alignment or resize'
    return result


def _role_verdict(comparison, requires_identical, band, changed_cap=None):
    if not comparison['sameDimensions']:
        return 'FAIL', 'dimensions differ'
    mean = comparison['meanAbsoluteChannelDifference']
    identical = comparison['identicalPixels']
    if requires_identical:
        if identical:
            return 'PASS', None
        return 'FAIL', f"pixels differ (max channel mean difference {max(mean):.4f})"
    if max(mean) > band:
        return 'FAIL', f'max channel mean difference {max(mean):.4f} exceeds band {band}'
    if changed_cap is not None and comparison['changedPixels'] > changed_cap:
        return 'FAIL', (f"{comparison['changedPixels']} changed pixels exceed "
            f"cap {changed_cap} (mean {max(mean):.4f} stayed within band {band})")
    return 'PASS', None


def check_screen(entry, capture_dir, baseline_dir, reference_dir):
    capture_dir, baseline_dir, reference_dir = Path(capture_dir), Path(baseline_dir), Path(reference_dir)
    capture_path = capture_dir / capture_filename(entry)
    baseline_path = baseline_dir / f"{entry['id']}.png"
    result = {'screen': entry['id'], 'title': entry.get('title', entry['id']),
        'capture': 'OK' if capture_path.is_file() else 'MISSING',
        'baseline': {'verdict': 'NOT_RUN', 'reason': 'no approved baseline'},
        'reference': {'verdict': 'NOT_RUN', 'reason': 'no approved source capture'},
        'verdict': 'PASS', 'reason': None}
    if result['capture'] == 'MISSING':
        result['verdict'], result['reason'] = 'FAIL', 'live capture missing'
        return result
    if entry.get('reference') is None:
        result['reference']['reason'] = 'no source capture registered'
    if baseline_path.is_file():
        comparison = compare_pair(baseline_path, capture_path)
        verdict, reason = _role_verdict(comparison,
            entry.get('requires_identical_pixels', False), entry.get('max_mean_abs_channel_difference'),
            entry.get('max_changed_pixels'))
        result['baseline'] = {'verdict': verdict, 'reason': reason,
            'meanAbsoluteChannelDifference': comparison['meanAbsoluteChannelDifference'],
            'identicalPixels': comparison['identicalPixels'], 'changedPixels': comparison['changedPixels'],
            'baselineSha256': comparison['images'][0]['originalSha256'],
            'captureSha256': comparison['images'][1]['originalSha256'],
            'size': comparison['images'][1]['size']}
        result['verdict'], result['reason'] = verdict, reason
    else:
        result['verdict'], result['reason'] = 'NOT_RUN', 'no approved baseline'
    reference_path = reference_dir / entry['reference'] if entry.get('reference') else None
    if reference_path is None or not reference_path.is_file():
        if entry.get('reference') is not None:
            result['reference']['reason'] = 'no approved source capture'
        return result
    comparison = compare_pair(reference_path, capture_path)
    verdict, reason = _role_verdict(comparison, False, entry['reference_tolerance'])
    result['reference'] = {'verdict': verdict, 'reason': reason,
        'meanAbsoluteChannelDifference': comparison['meanAbsoluteChannelDifference'],
        'identicalPixels': comparison['identicalPixels'],
        'referenceSha256': comparison['images'][0]['originalSha256'],
        'captureSha256': comparison['images'][1]['originalSha256']}
    if verdict == 'FAIL':
        result['verdict'], result['reason'] = 'FAIL', f'reference: {reason}'
    return result


def build_summary(results):
    counts = {'PASS': 0, 'FAIL': 0, 'NOT_RUN': 0}
    for result in results:
        counts[result['verdict']] = counts.get(result['verdict'], 0) + 1
    return {'verdict': 'FAIL' if counts['FAIL'] else 'PASS', 'counts': counts, 'screens': results}


def capture_plan(screens, build_dir, capture_dir):
    build_dir, capture_dir = Path(build_dir), Path(capture_dir)
    steps = {}
    for entry in screens:
        key = (entry['binary'], entry['case'])
        step = steps.setdefault(key, {'binary': str(build_dir / entry['binary']), 'case': entry['case'],
            'args': [entry['case']], 'env': {DATE_FIXTURE_ENV: '1'}, 'produces': []})
        capture = entry['capture']
        name = capture_filename(entry)
        if 'env' in capture:
            step['env'][capture['env']] = str(capture_dir / name)
        else:
            step['env'][capture['dir_env']] = str(capture_dir)
        step['produces'].append(name)
    return list(steps.values())


def run_capture(plan, timeout):
    failures = []
    for step in plan:
        if not Path(step['binary']).is_file():
            raise HarnessError(f'test binary not found: {step["binary"]}')
        environment = dict(os.environ)
        environment.update(step['env'])
        started = time.monotonic()
        finished = subprocess.run([step['binary'], *step['args']], env=environment,
            capture_output=True, text=True, timeout=timeout)
        elapsed = time.monotonic() - started
        status = 'ok' if finished.returncode == 0 else f'exit {finished.returncode}'
        print(f'  capture run {step["case"]}: {status} in {elapsed:.0f}s', file=sys.stderr)
        if finished.returncode != 0:
            tail = (finished.stdout + finished.stderr)[-800:]
            failures.append(f'{step["case"]}:\n{tail}')
    return failures


def _record():
    try:
        return subprocess.run(['git', '-C', str(REPO), 'rev-parse', 'HEAD'],
            capture_output=True, text=True, timeout=10).stdout.strip() or 'unknown'
    except OSError:
        return 'unknown'


def write_baselines(screens, capture_dir, baseline_dir):
    from PIL import Image
    baseline_dir = Path(baseline_dir)
    baseline_dir.mkdir(parents=True, exist_ok=True)
    document = {'generatedUtc': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()), 'repositoryRevision': _record(),
        'screens': {}}
    for entry in screens:
        source = Path(capture_dir) / capture_filename(entry)
        if not source.is_file():
            raise HarnessError(f'cannot approve a baseline without a capture: {entry["id"]}')
        target = baseline_dir / f"{entry['id']}.png"
        shutil.copyfile(source, target)
        with Image.open(target) as image:
            size = [image.width, image.height]
        document['screens'][entry['id']] = {'file': target.name, 'sha256': hashlib.sha256(target.read_bytes()).hexdigest(),
            'size': size, 'case': entry['case'], 'capture': entry['capture']}
    (baseline_dir / 'baselines.json').write_text(json.dumps(document, indent=2, sort_keys=True) + '\n')
    return document


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--manifest', type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument('--build', type=Path, help='directory containing native-window-test')
    parser.add_argument('--capture-dir', type=Path, required=True)
    parser.add_argument('--baseline-dir', type=Path, default=DEFAULT_BASELINE_DIR)
    parser.add_argument('--reference-dir', type=Path, default=DEFAULT_REFERENCE_DIR)
    parser.add_argument('--action', choices=('capture', 'baseline', 'check'), required=True)
    parser.add_argument('--reuse-captures', action='store_true',
        help='do not run the QtTest cases; compare the captures already in --capture-dir')
    parser.add_argument('--only', action='append', default=[], help='limit to a screen id (repeatable)')
    parser.add_argument('--timeout', type=int, default=900, help='seconds per QtTest case run')
    parser.add_argument('--json-out', type=Path)
    args = parser.parse_args(argv)

    try:
        screens = load_manifest(args.manifest)
        if args.only:
            unknown = set(args.only) - {entry['id'] for entry in screens}
            if unknown:
                raise HarnessError(f'unknown screen ids: {", ".join(sorted(unknown))}')
            screens = [entry for entry in screens if entry['id'] in args.only]
        args.capture_dir.mkdir(parents=True, exist_ok=True)
        if not args.reuse_captures:
            failures = run_capture(capture_plan(screens, args.build or Path('.'), args.capture_dir), args.timeout)
            if failures:
                print('capture runs failed:\n' + '\n'.join(failures), file=sys.stderr)
                return 1
        if args.action == 'capture':
            for entry in screens:
                print(f'{entry["id"]}: captured '
                    f'{(args.capture_dir / capture_filename(entry)).is_file()}')
            return 0
        if args.action == 'baseline':
            document = write_baselines(screens, args.capture_dir, args.baseline_dir)
            print(f'approved {len(document["screens"])} baselines into {args.baseline_dir}')
            return 0
        results = [check_screen(entry, args.capture_dir, args.baseline_dir, args.reference_dir)
            for entry in screens]
        summary = build_summary(results)
        for result in results:
            extra = f' ({result["reason"]})' if result.get('reason') else ''
            print(f'{result["verdict"]:>7}  {result["screen"]:<28} baseline={result["baseline"]["verdict"]}'
                f' reference={result["reference"]["verdict"]}{extra}')
        print(f'overall: {summary["verdict"]} ' + ' '.join(f'{key}={value}' for key, value in summary['counts'].items()))
        if args.json_out:
            args.json_out.parent.mkdir(parents=True, exist_ok=True)
            args.json_out.write_text(json.dumps(summary, indent=2) + '\n')
        return 3 if summary['verdict'] == 'FAIL' else 0
    except HarnessError as error:
        print(f'error: {error}', file=sys.stderr)
        return 1
    except ManifestError as error:
        print(f'manifest error: {error}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
