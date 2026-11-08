// Owned real transport. No HTTP client, credentials, QNetworkReply replacement or GOAWAY fault.
import tls from 'node:tls';
import http2 from 'node:http2';
import fs from 'node:fs';
import crypto from 'node:crypto';
import path from 'node:path';
const args = Object.fromEntries(process.argv.slice(2).map(value => {
    const i = value.indexOf('='); return [value.slice(0, i), value.slice(i + 1)];
}));
for (const key of ['key', 'cert', 'body', 'descriptor', 'trace', 'case', 'gate', 'host'])
    if (!args[key]) throw Error('Explicit argument required: ' + key);
if (!['127.0.0.1', '::1'].includes(args.host)) throw Error('Literal loopback only');
if ([args.descriptor, args.trace, args.gate].some(path => fs.existsSync(path))) throw Error('Fresh outputs only');
const allowed = ['two-fail', 'reset-cap', 'http-403', 'http-404', 'http-429', 'http-503', 'schema',
    'postheaders', 'tls', 'http1', 'cancel', 'destroy', 'replacement', 'deadline', 'ipv6', 'archive-policy'];
if (!allowed.includes(args.case)) throw Error('Unknown case');
const body = fs.readFileSync(args.body), sockets = new Set(), watchers = new Set();
let connections = 0, requests = 0, stopping = false, listenerPort = 0;
const sha = value => crypto.createHash('sha256').update(value).digest('hex');
const record = value => fs.appendFileSync(args.trace,
    JSON.stringify({...value, atNs: process.hrtime.bigint().toString()}) + '\n', {mode: 0o600});
function gateThen(socket, callback) {
    // Only a positive client public-signal/deadline receipt releases this immutable send/close.
    let consumed = false;
    const check = () => {
        if (consumed || !fs.existsSync(args.gate)) return;
        consumed = true; watcher.close(); watchers.delete(watcher);
        record({event: 'gate-observed', value: fs.readFileSync(args.gate, 'utf8').trim()});
        if (!socket.destroyed) callback();
    };
    const watcher = fs.watch(path.dirname(args.gate), check); watchers.add(watcher);
    check(); // Covers a signal atomically committed before the watcher was registered.
    socket.once('close', () => {if (watchers.delete(watcher)) watcher.close();});
}
function rawClose(socket, ordinal, connection, streamId, headersWritten = false, responseBytes = 0) {
    record({event: 'controlled-raw-tls-close', ordinal, connection, streamId,
        responseHeadersWritten: headersWritten, responseBytes,
        ownGoawayCalled: false, ownSessionDestroyCalled: false});
    socket.destroy();
}
const server = tls.createServer({key: fs.readFileSync(args.key), cert: fs.readFileSync(args.cert),
    ALPNProtocols: [args.case === 'http1' ? 'http/1.1' : 'h2'], requestCert: false}, socket => {
    const connection = ++connections; sockets.add(socket);
    record({event: 'tls-established', connection, alpn: socket.alpnProtocol,
        remoteIsLoopback: ['127.0.0.1', '::1'].includes(socket.remoteAddress)});
    socket.on('error', error => record({event: 'socket-error', connection, code: error.code || 'unknown'}));
    socket.on('close', () => {sockets.delete(socket); record({event: 'socket-close', connection});});
    if (args.case === 'http1') {
        let pending = '';
        socket.on('data', chunk => {
            pending += chunk.toString('ascii');
            if (pending.length > 8192) {record({event: 'invalid-request'}); socket.destroy(); return;}
            if (!pending.includes('\r\n\r\n')) return;
            const ordinal = ++requests;
            record({event: 'http1-request', ordinal, connection, requestLine: pending.split('\r\n')[0],
                noAuthorization: !/\r\nauthorization:/i.test(pending), noCookie: !/\r\ncookie:/i.test(pending)});
            if (ordinal > 4 || !pending.startsWith('GET /releases/latest HTTP/1.1\r\n'))
                record({event: 'invalid-request'});
            pending = ''; rawClose(socket, ordinal, connection, null);
        });
        return;
    }
    if (socket.alpnProtocol !== 'h2') {record({event: 'invalid-alpn'}); socket.destroy(); return;}
    const session = http2.performServerHandshake(socket);
    session.on('error', error => record({event: 'session-error', connection, code: error.code || 'unknown'}));
    session.on('stream', (stream, headers) => {
        const ordinal = ++requests, streamId = stream.id;
        const safe = {event: 'request-headers', ordinal, connection, streamId,
            method: headers[':method'], path: headers[':path'],
            noAuthorization: headers.authorization === undefined, noCookie: headers.cookie === undefined};
        record(safe);
        stream.on('error', error => record({event: 'stream-error', ordinal, code: error.code || 'unknown'}));
        stream.on('close', () => record({event: 'stream-close', ordinal}));
        if (safe.method !== 'GET' || safe.path !== '/releases/latest' || !safe.noAuthorization || !safe.noCookie) {
            record({event: 'invalid-request'}); socket.destroy(); return;
        }
        const fault = ordinal === 2 || (args.case === 'two-fail' && ordinal === 3)
            || (args.case === 'reset-cap' && [3, 4].includes(ordinal));
        if (args.case === 'deadline' && ordinal === 3) {
            record({event: 'recovery-held-without-headers', ordinal, connection, streamId}); return;
        }
        if (fault && args.case === 'postheaders') {
            const prefix = body.subarray(0, 9);
            stream.respond({':status': 200, 'content-type': 'application/json', 'content-length': String(body.length)});
            stream.write(prefix);
            record({event: 'body-prefix-enqueued', ordinal, bytes: prefix.length, sha256: sha(prefix)});
            gateThen(socket, () => rawClose(socket, ordinal, connection, streamId, true, prefix.length)); return;
        }
        if (fault && args.case.startsWith('http-')) {
            const status = Number(args.case.slice(5));
            stream.respond({':status': status, 'content-length': '0'}); stream.end();
            record({event: 'response-enqueued', ordinal, status, bytes: 0, sha256: sha(Buffer.alloc(0))}); return;
        }
        if (fault && args.case === 'schema') {
            const invalid = Buffer.from('{"tag_name":42}\n');
            stream.respond({':status': 200, 'content-type': 'application/json', 'content-length': String(invalid.length)});
            stream.end(invalid); record({event: 'response-enqueued', ordinal, status: 200, bytes: invalid.length, sha256: sha(invalid)}); return;
        }
        if (fault && !['ipv6', 'archive-policy', 'tls'].includes(args.case)) {
            if (args.case === 'deadline') gateThen(socket, () => rawClose(socket, ordinal, connection, streamId));
            else rawClose(socket, ordinal, connection, streamId);
            return;
        }
        const maximum = args.case === 'reset-cap' ? 5 : ['ipv6', 'archive-policy'].includes(args.case) ? 1 : 3;
        if (ordinal > maximum) {record({event: 'unexpected-extra-request', ordinal}); socket.destroy(); return;}
        let payload = body;
        if (args.case === 'archive-policy') payload = Buffer.from(JSON.stringify({tag_name: '0.1.5', draft: false, prerelease: false,
            assets: [{name: 'Exordos-Workspace-Native-0.1.5-linux-x86_64.tar.gz', size: 1,
                digest: 'sha256:' + '0'.repeat(64), browser_download_url: `https://127.0.0.1:${listenerPort}/archive`}]}));
        stream.respond({':status': 200, 'content-type': 'application/json', 'content-length': String(payload.length),
            'x-fixture-reply': String(ordinal)});
        stream.end(payload); record({event: 'response-enqueued', ordinal, status: 200, bytes: payload.length, sha256: sha(payload)});
    });
});
server.on('tlsClientError', error => record({event: 'tls-client-error', code: error.code || 'unknown'}));
server.on('error', error => {record({event: 'server-error', code: error.code || 'unknown'}); process.exitCode = 2;});
server.listen({host: args.host, port: 0, ipv6Only: args.host === '::1'}, () => {
    listenerPort = server.address().port;
    const hostPart = args.host === '::1' ? '[::1]' : args.host;
    fs.writeFileSync(args.descriptor + '.own-tmp', JSON.stringify({pid: process.pid, host: args.host, port: listenerPort,
        url: `https://${hostPart}:${listenerPort}/releases/latest`, bodyBytes: body.length, bodySha256: sha(body), node: process.version}) + '\n', {mode: 0o600});
    fs.renameSync(args.descriptor + '.own-tmp', args.descriptor);
});
function stop(signal) {
    if (stopping) return; stopping = true;
    record({event: 'owned-cleanup', signal, connections, requests});
    for (const watcher of watchers) watcher.close(); watchers.clear();
    server.close(() => process.exit(0)); for (const socket of sockets) socket.destroy();
}
process.on('SIGTERM', () => stop('SIGTERM')); process.on('SIGINT', () => stop('SIGINT'));
