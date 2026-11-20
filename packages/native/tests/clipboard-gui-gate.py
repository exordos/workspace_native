#!/usr/bin/env python3
"""Verify the CTest display classifications and the window clipboard gate."""
import json
import subprocess
import sys

build_dir = sys.argv[1]
def listing(*args):
    result = subprocess.run(['ctest', '--test-dir', build_dir, '--show-only=json-v1', *args],
                            check=True, capture_output=True, text=True)
    return json.loads(result.stdout)['tests']

tests = {test['name']: test for test in listing()}
for test in tests.values():
    labels = next((p['value'] for p in test.get('properties', []) if p['name'] == 'LABELS'), [])
    assert len({'gui', 'headless'}.intersection(labels)) == 1, (
        f"{test['name']} must have exactly one display classification, found {labels}")

name = 'native-clipboard-slow-ui'
assert name in tests, f'{name} is not registered'
properties = {p['name']: p['value'] for p in tests[name].get('properties', [])}
assert 'gui' in properties.get('LABELS', []), f'{name} is missing the gui label'
assert 'xdisplay' in properties.get('RESOURCE_LOCK', []), f'{name} does not lock xdisplay'
assert name not in {test['name'] for test in listing('-LE', 'gui')}, f'{name} runs in the headless gate'

pulse = tests.get('native-native-e2ee-pulse')
if pulse:
    assert any(p['name'] == 'RUN_SERIAL' and p['value'] for p in pulse['properties']), (
        'encrypted-read responsiveness must run without competing CTest processes')
