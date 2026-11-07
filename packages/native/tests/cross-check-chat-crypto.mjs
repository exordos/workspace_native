// Uses only fresh synthetic keys. Never point this helper at account/key storage.
import fs from 'node:fs/promises';
import path from 'node:path';
import os from 'node:os';
import { pathToFileURL } from 'node:url';
import { createHash } from 'node:crypto';
import { spawnSync } from 'node:child_process';

const args = process.argv.slice(2);
const option = (name) => { const i = args.indexOf(name); return i < 0 ? null : args[i + 1]; };
const executable = option('--native'), sourceRoot = option('--source-root'), dependencies = option('--dependency-root');
if (!executable || !sourceRoot || !dependencies) throw new Error('Required: --native, --source-root, --dependency-root; optional --vectors');
const check = (condition, description) => { if (!condition) throw new Error(description); };
const base64 = (bytes) => Buffer.from(bytes).toString('base64');
const bytes = (text) => new Uint8Array(Buffer.from(text, 'base64'));
const equal = (a, b) => Buffer.from(a).equals(Buffer.from(b));
const call = (requests) => {
  const child = spawnSync(executable, ['--interop'], { input: JSON.stringify(requests), encoding: 'utf8', maxBuffer: 32 * 1024 * 1024 });
  check(child.status === 0 && !child.error, 'Native interoperability process failed');
  const result = JSON.parse(child.stdout);
  check(Array.isArray(result) && result.length === requests.length, 'Invalid native response count');
  return result;
};
const expected = JSON.parse(await fs.readFile(path.join(sourceRoot, 'packages/web/package.json'), 'utf8')).dependencies;
const versions = {};
for (const name of ['@hpke/core', '@hpke/dhkem-x25519', '@hpke/chacha20poly1305', '@noble/ciphers', '@noble/hashes']) {
  versions[name] = JSON.parse(await fs.readFile(path.join(dependencies, name, 'package.json'), 'utf8')).version;
  check(versions[name] === expected[name], 'Dependency version differs from the source contract');
}
const source = await fs.readFile(path.join(sourceRoot, 'packages/web/src/entities/messenger/chat-real-crypto-provider.lib.ts'));
const esbuildPackage = JSON.parse(await fs.readFile(path.join(dependencies, 'esbuild/package.json'), 'utf8'));
const lock = JSON.parse(await fs.readFile(path.join(sourceRoot, 'package-lock.json'), 'utf8'));
check(esbuildPackage.version === lock.packages['node_modules/esbuild'].version, 'TypeScript transformer differs from source lockfile');
const esbuild = await import(pathToFileURL(path.join(dependencies, 'esbuild/lib/main.js')).href);
const transformed = await esbuild.transform(source.toString('utf8'), { loader: 'ts', format: 'esm', target: 'es2022' });
const temporary = await fs.mkdtemp(path.join(os.tmpdir(), 'cassi-native-crypto-source-'));
let report;
try {
  await fs.writeFile(path.join(temporary, 'package.json'), '{"type":"module","private":true}');
  await fs.writeFile(path.join(temporary, 'provider.mjs'), transformed.code);
  await fs.symlink(path.resolve(dependencies), path.join(temporary, 'node_modules'), 'dir');
  const { createRealChatCryptoProvider } = await import(pathToFileURL(path.join(temporary, 'provider.mjs')).href);
  const provider = createRealChatCryptoProvider();
  const groupKey = { bytes: Uint8Array.from({ length: 32 }, (_, n) => n) };
  const normal = { streamUuid: '10000000-0000-4000-8000-000000000001', keyUuid: '20000000-0000-4000-8000-000000000002' };
  const contexts = [normal, { streamUuid: 'строка"\n\u0000/👩🏽‍💻', keyUuid: 'key\\日本語\u2028' }];
  const samples = [new Uint8Array(), new TextEncoder().encode('Hello'), new TextEncoder().encode('Привет 👩🏽‍💻 — 日本語'),
    Uint8Array.from({ length: 256 }, (_, n) => n), new Uint8Array(160000).fill(120)];
  const vectors = { synthetic: true, provider: 'chat-real-crypto-provider.lib.ts', sourceSha256: createHash('sha256').update(source).digest('hex'), versions, messages: [], grants: [] };
  let checked = 0;
  for (const context of contexts) {
    for (const plaintext of samples) {
      const sealed = await provider.encryptMessage({ groupKey, plaintext, context }); check(sealed.ok, 'Source message encryption failed');
      const [opened, nativeSealed] = call([
        { operation: 'decryptMessage', ...context, groupKey: base64(groupKey.bytes), envelope: base64(sealed.value.bytes) },
        { operation: 'encryptMessage', ...context, groupKey: base64(groupKey.bytes), plaintext: base64(plaintext) },
      ]);
      check(opened.failure === 0 && equal(bytes(opened.bytes), plaintext), 'Source-to-native message mismatch');
      check(nativeSealed.failure === 0, 'Native message encryption failed');
      const sourceOpened = await provider.decryptMessage({ groupKey, envelope: { bytes: bytes(nativeSealed.bytes) }, context });
      check(sourceOpened.ok && equal(sourceOpened.value, plaintext), 'Native-to-source message mismatch');
      if (plaintext.length <= 1024)
        vectors.messages.push({ ...context, groupKey: base64(groupKey.bytes), plaintext: base64(plaintext), envelope: base64(sealed.value.bytes) });
      checked += 2;
    }
  }
  const generated = await provider.createRequestKeyPair(); check(generated.ok, 'Source key generation failed');
  const [nativePair] = call([{ operation: 'keypair' }]); check(nativePair.failure === 0, 'Native key generation failed');
  const pairs = [generated.value, { publicKey: { bytes: bytes(nativePair.publicKey) }, privateKey: bytes(nativePair.privateKey) }];
  for (const context of contexts) {
    const grantContext = { ...context, requestMessageUuid: '30000000-0000-4000-8000-000000000003', senderDeviceId: 'sender-device', recipientDeviceId: 'recipient-device' };
    for (const pair of pairs) {
      const sealed = await provider.encryptGroupKeyGrant({ groupKey, recipientPublicKey: pair.publicKey, context: grantContext });
      check(sealed.ok && sealed.value.bytes.length === 85, 'Source grant encryption failed');
      const [opened, nativeSealed] = call([
        { operation: 'decryptGrant', ...grantContext, privateKey: base64(pair.privateKey), publicKey: base64(pair.publicKey.bytes), envelope: base64(sealed.value.bytes) },
        { operation: 'encryptGrant', ...grantContext, groupKey: base64(groupKey.bytes), publicKey: base64(pair.publicKey.bytes) },
      ]);
      check(opened.failure === 0 && equal(bytes(opened.bytes), groupKey.bytes), 'Source-to-native HPKE grant mismatch');
      check(nativeSealed.failure === 0, 'Native grant encryption failed');
      const sourceOpened = await provider.decryptGroupKeyGrant({ requestKeyPair: pair, envelope: { bytes: bytes(nativeSealed.bytes) }, context: grantContext });
      check(sourceOpened.ok && equal(sourceOpened.value.bytes, groupKey.bytes), 'Native-to-source HPKE grant mismatch');
      vectors.grants.push({ ...grantContext, groupKey: base64(groupKey.bytes), privateKey: base64(pair.privateKey), publicKey: base64(pair.publicKey.bytes), envelope: base64(sealed.value.bytes) }); checked += 2;
    }
  }
  if (option('--vectors')) await fs.writeFile(option('--vectors'), JSON.stringify(vectors, null, 2) + '\n', { mode: 0o600 });
  report = { pass: true, synthetic: true, checkedDirections: checked, sourceSha256: vectors.sourceSha256, versions,
    transformer: { name: 'esbuild', version: esbuildPackage.version, sourceLockfileMatched: true },
    cases: 'Both directions: empty, ASCII, UTF8, binary,160kB messages; normal/escaped Unicode context; HPKE with source/native-generated X25519 pairs' };
} finally {
  await fs.rm(temporary, { recursive: true, force: true });
}
console.log(JSON.stringify({ ...report, sourceCopyRemoved: true }));
