import fs from 'node:fs';
import path from 'node:path';
import { execFileSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';
if (process.argv.length !== 3) throw new Error('Usage: node build-peer.mjs <workspace_ui_checkout>');
const repo=path.resolve(process.argv[2]),ref='95495b10b828f853c9e770b47216073eb973fa95';
const here=path.dirname(fileURLToPath(import.meta.url));
const esbuild=createRequire(path.join(repo,'package.json'))('esbuild');
const sources={};
const load=(file)=>{const bytes=execFileSync('git',['show',ref+':'+file],{cwd:repo});sources[file]=createHash('sha256').update(bytes).digest('hex');return bytes.toString('utf8');};
const versions={}, expected=JSON.parse(load('packages/web/package.json')).dependencies;
for(const name of ['@hpke/core','@hpke/dhkem-x25519','@hpke/chacha20poly1305','@noble/ciphers','@noble/hashes']){
 const value=JSON.parse(fs.readFileSync(path.join(repo,'node_modules',name,'package.json'))).version;
 if(value!==expected[name])throw new Error('Pinned dependency mismatch: '+name);versions[name]=value;
}
const locked = JSON.parse(load('package-lock.json')).packages['node_modules/esbuild'].version;
if(esbuild.version !== locked) throw new Error('Pinned esbuild version mismatch');
versions.esbuild = esbuild.version;
await esbuild.build({absWorkingDir:here,nodePaths:[path.join(repo,'node_modules')],entryPoints:['peer.ts'],outfile:'peer.mjs',bundle:true,platform:'node',format:'esm',target:'es2022',plugins:[{name:'pinned-source',setup(build){
 build.onResolve({filter:/^pinned:/},args=>({path:args.path.slice(7),namespace:'pinned'}));
 build.onResolve({filter:/^~\//},args=>{
  if(args.path==='~/shared/lib/device-name.lib')return{path:'device-name',namespace:'platform'};
  return{path:'packages/web/src/'+args.path.slice(2)+'.ts',namespace:'pinned'};
 });
 build.onResolve({filter:/^\./,namespace:'pinned'},args=>({path:path.posix.normalize(path.posix.join(path.posix.dirname(args.importer),args.path))+'.ts',namespace:'pinned'}));
 build.onLoad({filter:/.*/,namespace:'pinned'},args=>({contents:load(args.path),loader:'ts',resolveDir:repo}));
 build.onLoad({filter:/.*/,namespace:'platform'},()=>({contents:'export const getDeviceName=async()=>"Synthetic JS issuer";',loader:'js'}));
}}]});
const fixtureSource=fs.readFileSync(path.join(here,'peer.ts'));
const legacyStart=Buffer.from('// BEGIN ORIGINAL ONE-SHOT BODY\n');
const legacyEnd=Buffer.from('// END ORIGINAL ONE-SHOT BODY\n');
const start=fixtureSource.indexOf(legacyStart),end=fixtureSource.indexOf(legacyEnd);
if(start<0||end<=start)throw new Error('Original one-shot fixture witness missing');
const fixtures={entrySha256:createHash('sha256').update(fixtureSource).digest('hex'),
 bundleSha256:createHash('sha256').update(fs.readFileSync(path.join(here,'peer.mjs'))).digest('hex'),
 legacyOneShotBodySha256:createHash('sha256').update(fixtureSource.subarray(start+legacyStart.length,end)).digest('hex'),
 protocols:['legacy-one-shot-eof','optional-two-line-public-envelope','optional-two-line-native-key-response','optional-two-line-source-mixed-outcomes-pair','optional-native-key-response-public-probe']};
fs.writeFileSync(path.join(here,'peer-provenance.json'),JSON.stringify({ref,sources,versions,fixtures,
 platformAdapter:'Only getDeviceName; original grant/reject modes do not call it; independent recipient request uses fixed synthetic label'},null,2)+'\n');
