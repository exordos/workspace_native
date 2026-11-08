#!/usr/bin/env python3
"""Real local TLS regression; builds nothing, preserves HOME, retains raw warnings/artifacts."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import socket
import subprocess
import tempfile
import time

CASES = ['recovery', 'two-fail', 'reset-cap', 'http-403', 'http-404', 'http-429', 'http-503',
         'schema', 'postheaders', 'tls', 'http1', 'cancel', 'destroy', 'replacement', 'deadline',
         'ipv6', 'archive-policy', 'endpoint-admission']

def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()

def group_members(pgid):
    found = []
    for path in Path('/proc').iterdir():
        if not path.name.isdigit():
            continue
        try:
            if os.getpgid(int(path.name)) == pgid:
                found.append(int(path.name))
        except ProcessLookupError:
            pass
    return found

def child_identity(pid):
    path = Path('/proc') / str(pid)
    # stat comm can contain spaces: start time is field 22 after the closing ')'.
    fields = (path / 'stat').read_text().rsplit(')', 1)[1].split()
    return (path.stat().st_uid, fields[19], os.getpgid(pid))

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--case', choices=CASES, required=True)
    parser.add_argument('--driver', type=Path, required=True)
    parser.add_argument('--recovery-driver', type=Path, required=True)
    parser.add_argument('--node', default='node')
    parser.add_argument('--openssl', default='openssl')
    parser.add_argument('--output-root', type=Path, required=True)
    parser.add_argument('--runtime-prefix', type=Path)
    parser.add_argument('--runtime-reference', type=Path)
    parser.add_argument('--updater-source-dir', type=Path, required=True)
    args = parser.parse_args()
    own = Path(__file__).resolve().parent
    source_paths = [Path(__file__), own / 'fixture.mjs', own / 'recovery-fixture.mjs', own / 'metadata.json',
                    args.driver.resolve(), args.recovery_driver.resolve(),
                    args.updater_source_dir.resolve() / 'native-updater.cpp', args.updater_source_dir.resolve() / 'native-updater.h']
    initial_inputs = {str(path): sha(path) for path in source_paths}
    args.output_root.mkdir(parents=True, exist_ok=True)
    out = Path(tempfile.mkdtemp(prefix=args.case + '-', dir=args.output_root.resolve()))
    os.chmod(out, 0o700)
    private = out / 'private-keys'; private.mkdir(mode=0o700)
    envdir = out / 'private-environment'
    env = dict(os.environ)
    original_home = env.get('HOME')
    for key in ['XDG_CONFIG_HOME', 'XDG_CACHE_HOME', 'XDG_DATA_HOME', 'XDG_STATE_HOME', 'XDG_RUNTIME_DIR', 'TMPDIR']:
        path = envdir / key.lower(); path.mkdir(parents=True, mode=0o700); env[key] = str(path)
    # Default Qt warning output is part of the assertion. Never filter an expected/extra warning.
    env.pop('QT_LOGGING_RULES', None); env.pop('QT_MESSAGE_PATTERN', None)
    assert env.get('HOME') == original_home
    procs = []; records = []; server = None; port = None
    host = '::1' if args.case == 'ipv6' else '127.0.0.1'
    observed = False; failure = None; summary = None; trace = []
    cleaning = False; spawning = False; interrupted_signals = []
    def interrupted(signum, _frame):
        interrupted_signals.append(signum)
        if not cleaning and not spawning:
            raise InterruptedError('Owned test wrapper received signal ' + str(signum))
    old_handlers = {number: signal.signal(number, interrupted) for number in [signal.SIGTERM, signal.SIGINT]}
    def spawn(command, label, child_env=None):
        nonlocal spawning
        stdout = (out / (label + '.stdout')).open('wb')
        stderr = (out / (label + '.stderr')).open('wb')
        spawning = True  # Defer an interruption until this exact child is registered for cleanup.
        try:
            proc = subprocess.Popen(list(map(str, command)), stdout=stdout, stderr=stderr,
                                    env=child_env or env, start_new_session=True)
            identity = child_identity(proc.pid)
            procs.append((proc, identity, label))
        finally:
            stdout.close(); stderr.close()
            spawning = False
        if interrupted_signals:
            raise InterruptedError('Signal received during owned child registration')
        return proc
    def reap(proc, identity):
        if proc.poll() is None:
            assert child_identity(proc.pid) == identity and identity[0] == os.getuid() and identity[2] == proc.pid
            os.killpg(proc.pid, signal.SIGTERM)
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                assert child_identity(proc.pid) == identity
                os.killpg(proc.pid, signal.SIGKILL); proc.wait(timeout=5)
        # Never signal a name-selected process or a group after its leader was reaped.
        return group_members(proc.pid)
    def step(command, label, seconds, child_env=None):
        started = time.monotonic(); proc = spawn(command, label, child_env)
        timed_out = False
        try:
            code = proc.wait(timeout=seconds)
        except subprocess.TimeoutExpired:
            timed_out = True; reap(proc, procs[-1][1]); code = proc.returncode
        records.append({'label': label, 'command': list(map(str, command)), 'pid': proc.pid, 'pgid': proc.pid,
                        'exitCode': code, 'timeout': timed_out, 'wallSeconds': round(time.monotonic() - started, 3)})
        assert code == 0 and not timed_out, records[-1]
        assert not group_members(proc.pid), 'Owned child left descendants'
        return proc
    try:
        binary = args.recovery_driver.resolve() if args.case == 'recovery' else args.driver.resolve()
        assert binary.is_file() and os.access(binary, os.X_OK)
        runtime_env = dict(env)
        prefix = None; reference = None
        if args.runtime_prefix:
            prefix = args.runtime_prefix.resolve()
            assert args.runtime_reference, 'A runtime prefix needs frozen selected mapping hashes'
            reference = json.loads(args.runtime_reference.read_text())['referenceReleasedRuntime']
            assert prefix == Path(reference['prefix']).resolve()
            for entry in reference['selectedFiles']:
                assert Path(entry['path']).resolve().is_relative_to(prefix) and sha(entry['path']) == entry['sha256']
            isolated = out / 'isolated-client'; isolated.mkdir()
            shutil.copy2(binary, isolated / binary.name); binary = isolated / binary.name
            (isolated / 'qt.conf').write_text('[Paths]\nPrefix=' + str(prefix) + '\nLibraries=lib\nPlugins=lib/x86_64-linux-gnu/qt6/plugins\n')
            runtime_env.update(LD_LIBRARY_PATH=str(prefix / 'lib'), QT_PLUGIN_PATH=str(prefix / 'lib/x86_64-linux-gnu/qt6/plugins'))
        if args.case == 'endpoint-admission':
            command = [binary, '--case', args.case]
        else:
            # These are new ephemeral owned fixture keys only, generated at the future actual run.
            step([args.openssl, 'req', '-x509', '-newkey', 'rsa:2048', '-noenc', '-keyout', private / 'ca.key',
                  '-out', out / 'ca.pem', '-days', '1', '-subj', '/CN=Cassi owned updater H2 test CA',
                  '-addext', 'basicConstraints=critical,CA:TRUE', '-addext', 'keyUsage=critical,keyCertSign,cRLSign'], 'ca', 20)
            step([args.openssl, 'req', '-new', '-newkey', 'rsa:2048', '-noenc', '-keyout', private / 'server.key',
                  '-out', private / 'server.csr', '-subj', '/CN=owned updater fixture'], 'csr', 20)
            san = 'IP:127.0.0.2' if args.case == 'tls' else 'IP:127.0.0.1,IP:::1'
            (private / 'extensions.cnf').write_text('basicConstraints=critical,CA:FALSE\nkeyUsage=critical,digitalSignature,keyEncipherment\n'
                                                  'extendedKeyUsage=serverAuth\nsubjectAltName=' + san + '\n')
            step([args.openssl, 'x509', '-req', '-in', private / 'server.csr', '-CA', out / 'ca.pem',
                  '-CAkey', private / 'ca.key', '-set_serial', '1', '-out', out / 'server.pem', '-days', '1',
                  '-extfile', private / 'extensions.cnf'], 'cert', 20)
            fixture = own / ('recovery-fixture.mjs' if args.case == 'recovery' else 'fixture.mjs')
            command = [args.node, fixture, 'key=' + str(private / 'server.key'), 'cert=' + str(out / 'server.pem'),
                       'body=' + str(own / 'metadata.json'), 'descriptor=' + str(out / 'server.json'), 'trace=' + str(out / 'server.jsonl')]
            if args.case != 'recovery':
                command += ['case=' + args.case, 'gate=' + str(out / 'public-signal.gate'), 'host=' + host]
            server = spawn(command, 'server')
            deadline = time.monotonic() + 10
            while not (out / 'server.json').exists():
                assert server.poll() is None and time.monotonic() < deadline, 'Local server startup failed'
                time.sleep(.01)  # Descriptor readiness polling; no application fault/retry stimulus.
            descriptor = json.loads((out / 'server.json').read_text()); port = descriptor['port']
            assert descriptor['pid'] == server.pid and descriptor['host'] == host
            assert descriptor['bodySha256'] == sha(own / 'metadata.json')
            if args.case == 'recovery':
                command = [binary, '--url', descriptor['url'], '--ca', out / 'ca.pem', '--expected', own / 'metadata.json']
            else:
                command = [binary, '--case', args.case, '--url', descriptor['url'], '--ca', out / 'ca.pem',
                           '--body', own / 'metadata.json', '--gate', out / 'public-signal.gate']
        step(command, 'client', 70, runtime_env)
        events = [json.loads(line) for line in (out / 'client.stdout').read_text().splitlines() if line.startswith('{')]
        summaries = [event for event in events if event.get('event') == 'summary']
        assert len(summaries) == 1
        summary = summaries[0]
        if args.case == 'recovery':
            assert summary['status'] == 'RECOVERY_ORACLE_GREEN' and summary['oracleComplete'] and summary['warmupCurrent']
            assert summary['publicChecks'] == 2 and len(summary['wireReplies']) == 3 and summary['automaticRecoveryObserved']
            assert not summary['manualThirdCheckIssued'] and summary['sslErrors'] == []
        else:
            assert summary['passed'] is True, summary
        if args.case != 'endpoint-admission':
            assert summary['qtRuntimeVersion'] == '6.10.2', 'Different Qt version needs explicit review'
            maps = summary['runtimeMappings']
            for name in ['libQt6Core.so', 'libQt6Network.so', 'libssl.so', 'libcrypto.so', 'libqopensslbackend.so']:
                found = [entry for entry in maps if name in Path(entry['path']).name]
                assert len(found) == 1 and sha(found[0]['path']) == found[0]['sha256']
                if prefix:
                    assert Path(found[0]['path']).resolve().is_relative_to(prefix)
                    frozen = [entry for entry in reference['selectedFiles'] if Path(entry['path']).resolve() == Path(found[0]['path']).resolve()]
                    assert len(frozen) == 1 and frozen[0]['sha256'] == found[0]['sha256']
        observed = True
    except BaseException as error:
        failure = type(error).__name__ + ': ' + str(error)
    finally:
        cleaning = True
        cleanup_errors = []
        for proc, identity, label in reversed(procs):
            try:
                assert not reap(proc, identity), 'Residual owned group ' + str(proc.pid)
                if label == 'server':
                    assert proc.returncode == 0, 'Server abnormal cleanup'
            except BaseException as error:
                cleanup_errors.append(label + ': ' + str(error))
        shutil.rmtree(private); shutil.rmtree(envdir)
        port_closed = None
        if port:
            with socket.socket(socket.AF_INET6 if host == '::1' else socket.AF_INET, socket.SOCK_STREAM) as probe:
                probe.settimeout(.25); port_closed = probe.connect_ex((host, port)) != 0
        try:
            trace = [json.loads(line) for line in (out / 'server.jsonl').read_text().splitlines()] if port else []
            assert not any(e['event'] in ['invalid-request', 'invalid-alpn', 'unexpected-extra-request', 'server-error'] for e in trace)
            requests = [e for e in trace if e['event'] == 'request-headers']
            closes = [e for e in trace if e['event'] == 'controlled-raw-tls-close']
            assert all(e['noAuthorization'] and e['noCookie'] and e['method'] == 'GET' and e['path'] == '/releases/latest' for e in requests)
            expected_count = {'recovery': 3, 'two-fail': 3, 'reset-cap': 5, 'cancel': 2, 'destroy': 2,
                              'replacement': 3, 'deadline': 3, 'ipv6': 1, 'archive-policy': 1, 'tls': 0}.get(args.case, 2)
            if args.case == 'http1':
                h1 = [e for e in trace if e['event'] == 'http1-request']
                assert 1 <= len(h1) <= 4 and all(e['noAuthorization'] and e['noCookie'] for e in h1)
                assert summary['publicChecks'] == 1 and len(summary['wireReplies']) == 1
            elif args.case != 'endpoint-admission':
                assert len(requests) == expected_count, requests
                if expected_count >= 2:
                    assert [(e['connection'], e['streamId']) for e in requests[:2]] == [(1, 1), (1, 3)]
                if args.case in ['recovery', 'replacement', 'two-fail', 'reset-cap', 'deadline']:
                    assert requests[2]['connection'] == 2 and requests[2]['streamId'] == 1
                established = [e for e in trace if e['event'] == 'tls-established']
                assert all(e['alpn'] == 'h2' and e['remoteIsLoopback'] for e in established)
                expected_closes = {'recovery': 1, 'two-fail': 2, 'reset-cap': 3, 'cancel': 1, 'destroy': 1,
                                   'replacement': 1, 'deadline': 1, 'postheaders': 1}.get(args.case, 0)
                assert len(closes) == expected_closes
                assert all(not e['ownGoawayCalled'] and not e['ownSessionDestroyCalled'] for e in closes)
                if args.case == 'postheaders':
                    assert closes[0]['responseHeadersWritten'] and closes[0]['responseBytes'] == 9
                    assert any(e['event'] == 'gate-observed' and e['value'] == 'actual-status200-and-readyRead' for e in trace)
                else:
                    assert all(not e['responseHeadersWritten'] and e['responseBytes'] == 0 for e in closes)
                if args.case == 'deadline':
                    assert any(e['event'] == 'gate-observed' and e['value'] == 'second-check-still-checking-at-26s' for e in trace)
                    assert any(e['event'] == 'recovery-held-without-headers' for e in trace)
                    receipts = [e for e in events if e.get('event') == 'public-gate-released']
                    assert len(receipts) == 1 and receipts[0]['atMs'] >= 26000
                for response in [e for e in trace if e['event'] == 'response-enqueued']:
                    if args.case == 'schema' and response['ordinal'] == 2:
                        expected = b'{"tag_name":42}\n'
                    elif args.case == 'archive-policy':
                        expected = json.dumps({'tag_name': '0.1.5', 'draft': False, 'prerelease': False, 'assets': [{
                            'name': 'Exordos-Workspace-Native-0.1.5-linux-x86_64.tar.gz', 'size': 1,
                            'digest': 'sha256:' + '0' * 64, 'browser_download_url': f'https://127.0.0.1:{port}/archive'}]}, separators=(',', ':')).encode()
                    elif args.case.startswith('http-') and response['ordinal'] == 2:
                        expected = b''
                    else:
                        expected = (own / 'metadata.json').read_bytes()
                    assert response['bytes'] == len(expected) and response['sha256'] == hashlib.sha256(expected).hexdigest()
            expected_warnings = []
            if args.case != 'http1':
                for close in closes:
                    stream = close['streamId']
                    expected_warnings += [f'qt.network.http2: stream {stream} error: "Connection closed"',
                                          f'qt.network.http2: stream {stream} finished with error: "HTTP/2 protocol error"']
            assert (out / 'client.stderr').read_text().splitlines() == expected_warnings, 'Unexpected/missing raw Qt warning'
            if server:
                assert (out / 'server.stderr').read_bytes() == b'' and port_closed is True
        except BaseException as error:
            failure = (failure + '; ' if failure else '') + 'Trace/warning: ' + str(error)
        final_inputs = {str(path): sha(path) for path in source_paths if path.is_file()}
        if final_inputs != initial_inputs:
            failure = (failure + '; ' if failure else '') + 'Source or executable changed during test'
        if interrupted_signals:
            failure = (failure + '; ' if failure else '') + 'Interrupted signals: ' + str(interrupted_signals)
        result = {'status': 'PASS' if observed and not failure and not cleanup_errors else 'NOT_ACCEPTED', 'case': args.case,
                  'failure': failure, 'cleanupErrors': cleanup_errors, 'portClosed': port_closed,
                  'homeWasPreserved': env.get('HOME') == original_home, 'privateKeysRemoved': not private.exists(),
                  'privateXdgRemoved': not envdir.exists(), 'remainingGroups': {str(p.pid): group_members(p.pid) for p, _, _ in procs},
                  'interruptedSignals': interrupted_signals,
                  'steps': records, 'summary': summary, 'serverTrace': trace,
                  'ownedProcesses': [{'pid': p.pid, 'uid': identity[0], 'startTime': identity[1], 'pgid': identity[2],
                                     'label': label, 'exitCode': p.returncode} for p, identity, label in procs],
                  'inputHashesBefore': initial_inputs, 'inputHashesAfter': final_inputs, 'sourceAndExecutableUnchanged': final_inputs == initial_inputs,
                  'outputHashes': {p.name: sha(p) for p in out.iterdir() if p.is_file() and p.name != 'RESULT.json'}}
        (out / 'RESULT.json').write_text(json.dumps(result, indent=2) + '\n')
        print(json.dumps({'artifactDirectory': str(out), 'status': result['status'], 'case': args.case, 'failure': failure}))
        if result['status'] != 'PASS':
            print(json.dumps(result, indent=2))
        for number, handler in old_handlers.items():
            signal.signal(number, handler)
    assert result['status'] == 'PASS', result['failure'] or result['cleanupErrors']

if __name__ == '__main__':
    main()
