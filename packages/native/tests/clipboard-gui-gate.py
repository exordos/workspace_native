#!/usr/bin/env python3
"""Ensure the clipboard window regression stays out of the headless gate."""
import json
import subprocess
import sys

build_dir = sys.argv[1]
def listing(*args):
    result = subprocess.run(['ctest', '--test-dir', build_dir, '--show-only=json-v1', *args],
                            check=True, capture_output=True, text=True)
    return json.loads(result.stdout)['tests']

tests = {test['name']: test for test in listing()}
name = 'native-clipboard-slow-ui'
assert name in tests, f'{name} is not registered'
properties = {p['name']: p['value'] for p in tests[name].get('properties', [])}
assert 'gui' in properties.get('LABELS', []), f'{name} is missing the gui label'
assert 'xdisplay' in properties.get('RESOURCE_LOCK', []), f'{name} does not lock xdisplay'
assert name not in {test['name'] for test in listing('-LE', 'gui')}, f'{name} runs in the headless gate'
