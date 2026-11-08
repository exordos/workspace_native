import fs from 'node:fs';
import { createRealChatCryptoProvider } from 'pinned:packages/web/src/entities/messenger/chat-real-crypto-provider.lib.ts';
import { ChatE2eeController } from 'pinned:packages/web/src/entities/messenger/chat-e2ee-controller.lib.ts';
import { decodeChatE2eePayload, encodeChatE2eePayload } from 'pinned:packages/web/src/entities/messenger/chat-e2ee-payload-adapters.lib.ts';
import { createMessengerPlacementUuid } from 'pinned:packages/web/src/entities/messenger/messenger-message-identity.lib.ts';
// Optional test protocol. The normal one-shot mode below is retained verbatim.
// Only public envelopes and synthetic expected text cross this pipe; the source
// group key stays in this process from grant through native-send verification.
async function verifyNativeSendMode() {
 const maximumLineBytes=512*1024;
 async function* frames() {
  let buffered=Buffer.alloc(0);
  for await(const chunk of process.stdin) {
   const bytes=Buffer.isBuffer(chunk)?chunk:Buffer.from(chunk);
   if(buffered.length+bytes.length>maximumLineBytes+65536)throw new Error('Synthetic peer frame too large');
   buffered=Buffer.concat([buffered,bytes]);
   for(let end;(end=buffered.indexOf(10))!==-1;) {
    if(end===0||end>maximumLineBytes)throw new Error('Synthetic peer frame invalid');
    const line=buffered.subarray(0,end); buffered=buffered.subarray(end+1);
    yield JSON.parse(line.toString('utf8'));
   }
   if(buffered.length>maximumLineBytes)throw new Error('Synthetic peer frame too large');
  }
  if(buffered.length!==0)throw new Error('Synthetic peer frame incomplete');
 }
 const iterator=frames();
 const first=await iterator.next(); if(first.done)throw new Error('Synthetic peer input missing');
 const input=first.value;
 if(input.reject||typeof input.plaintext!=='string'||typeof input.targetTopic!=='string'
  ||(input.readPlaintext!==undefined&&typeof input.readPlaintext!=='string'))
  throw new Error('Synthetic peer input invalid');
 const provider=createRealChatCryptoProvider();
 const key=await provider.createGroupKey(); if(!key.ok)throw new Error('Synthetic peer key unavailable');
 const expected=new TextEncoder().encode(input.plaintext);
 const readPlaintext=new TextEncoder().encode(input.readPlaintext??input.plaintext);
 let controller:ChatE2eeController|undefined;
 let cleartext:Uint8Array|undefined;
 try {
  const owner={accountId:'synthetic',instanceId:'synthetic',organizationId:'synthetic',projectId:input.project,userUuid:input.author};
  const scope={owner,deviceId:input.senderDevice,streamUuid:input.stream,keyUuid:input.key,runtimeGeneration:1};
  const manager=input.role==='owner'||input.role==='administrator';
  controller=new ChatE2eeController({scope,device:{deviceId:scope.deviceId,deviceName:'Synthetic JS issuer'},provider,
   storage:{readChatKey:async()=>({groupKey:key.value}),readChatKeyDeletionRevision:async()=>0},
   getCurrentScope:()=>scope,getRights:()=>({canRequest:true,canSend:true,canGrant:manager,canReject:manager,canRotate:input.role==='owner'}),
   isIssuerAllowed:()=>manager,isAnnouncerAllowed:()=>manager,isRequesterAllowed:()=>true,
   sendPayload:async(payload,guard)=>{if(!guard())throw new Error('Synthetic peer denied');return{uuid:crypto.randomUUID(),streamUuid:input.stream,authorUuid:input.author,payload};}
  });
  const request={uuid:input.request,streamUuid:input.stream,authorUuid:input.requester,payload:{kind:'key_request' as const,keyUuid:input.key,
   deviceId:input.recipientDevice,deviceName:'Synthetic native device',publicKey:{bytes:new Uint8Array(Buffer.from(input.publicKey,'base64'))}}};
  const response=await controller.grantKey(request);
  if(!response.ok)throw new Error('Synthetic peer grant unavailable');
  const encrypted=await provider.encryptMessage({groupKey:key.value,plaintext:readPlaintext,context:{streamUuid:input.stream,keyUuid:input.key}});
  if(!encrypted.ok)throw new Error('Synthetic ciphertext unavailable');
  process.stdout.write(JSON.stringify({ok:true,readyForNativeSend:true,
   message:{uuid:response.value.uuid,author_uuid:input.author,payload:encodeChatE2eePayload(response.value.payload)},
   encrypted:{kind:'e2ee',key_uuid:input.key,content:Buffer.from(encrypted.value.bytes).toString('base64')}})+'\n');
  const second=await iterator.next(); if(second.done)throw new Error('Synthetic native envelope missing');
  const frame=second.value,payload=frame.payload;
  if(frame.kind!=='verify-native-send'||frame.stream!==input.stream||frame.topic!==input.targetTopic
   ||!payload||Object.keys(payload).sort().join(',')!=='content,key_uuid,kind'||payload.kind!=='e2ee'
   ||payload.key_uuid!==input.key||typeof payload.content!=='string'||payload.content.length>213336
   ||!/^(?:[A-Za-z0-9+/]{4})*(?:[A-Za-z0-9+/]{2}==|[A-Za-z0-9+/]{3}=)?$/.test(payload.content))
   throw new Error('Synthetic native envelope invalid');
  const envelope=Buffer.from(payload.content,'base64');
  if(envelope.length<48||envelope.toString('base64')!==payload.content)throw new Error('Synthetic native envelope invalid');
  const result=await provider.decryptMessage({groupKey:key.value,envelope:{bytes:new Uint8Array(envelope)},
   context:{streamUuid:input.stream,keyUuid:input.key}});
  if(!result.ok)throw new Error('Synthetic native envelope could not decrypt');
  cleartext=result.value;
  const matchesExpected=cleartext.length===expected.length&&cleartext.every((byte,index)=>byte===expected[index]);
  if(!(await iterator.next()).done)throw new Error('Synthetic peer extra frame');
  process.stdout.write(JSON.stringify({ok:true,matchesExpected,plaintextBytes:cleartext.length,keyUuid:input.key})+'\n');
 } finally {
  cleartext?.fill(0);expected.fill(0);readPlaintext.fill(0);key.value.bytes.fill(0);controller?.invalidate();
 }
}

// Optional reverse-direction fixture. A separate actual Source recipient owns
// its pending pair; neither that pair nor either group secret crosses stdout.
async function verifyNativeKeyResponseMode(mixedOutcomes = false) {
 const maximumLineBytes=512*1024;
 async function* frames() {
  let buffered=Buffer.alloc(0);
  for await(const chunk of process.stdin) {
   const bytes=Buffer.isBuffer(chunk)?chunk:Buffer.from(chunk);
   if(buffered.length+bytes.length>maximumLineBytes+65536)throw new Error('Synthetic peer frame too large');
   buffered=Buffer.concat([buffered,bytes]);
   for(let end;(end=buffered.indexOf(10))!==-1;) {
    if(end===0||end>maximumLineBytes)throw new Error('Synthetic peer frame invalid');
    const line=buffered.subarray(0,end);buffered=buffered.subarray(end+1);
    yield JSON.parse(line.toString('utf8'));
   }
   if(buffered.length>maximumLineBytes)throw new Error('Synthetic peer frame too large');
  }
  if(buffered.length!==0)throw new Error('Synthetic peer frame incomplete');
 }
 const iterator=frames(),first=await iterator.next();if(first.done)throw new Error('Synthetic peer input missing');
 const input=first.value;
 if(typeof input.plaintext!=='string'||typeof input.seedNative!=='boolean'
  ||typeof input.requestTopic!=='string'||typeof input.targetTopic!=='string'
  ||input.sourceRecipient===input.requester||input.sourceDevice===input.nativeDevice)
  throw new Error('Synthetic independent recipient input invalid');
 const provider=createRealChatCryptoProvider();
 const key=await provider.createGroupKey();if(!key.ok)throw new Error('Synthetic source key unavailable');
 const plaintext=new TextEncoder().encode(input.plaintext);
 const pending=new Map<string,any>(),completions=new Map<string,any>(),pairs:any[]=[];
 let recipientKey:any=null,issuer:ChatE2eeController|undefined,recipient:ChatE2eeController|undefined;
 let decrypted:Uint8Array|undefined;
 try {
  const owner={accountId:'synthetic',instanceId:'synthetic',organizationId:'synthetic',projectId:input.project,userUuid:input.author};
  const scope={owner,deviceId:input.senderDevice,streamUuid:input.stream,keyUuid:input.key,runtimeGeneration:1};
  issuer=new ChatE2eeController({scope,device:{deviceId:scope.deviceId,deviceName:'Synthetic JS issuer'},provider,
   storage:{readChatKey:async()=>({groupKey:key.value}),readChatKeyDeletionRevision:async()=>0},
   getCurrentScope:()=>scope,getRights:()=>({canRequest:true,canSend:true,canGrant:true,canReject:true,canRotate:false}),
   isIssuerAllowed:()=>true,isAnnouncerAllowed:()=>true,isRequesterAllowed:()=>true,
   sendPayload:async(payload,guard)=>{if(!guard())throw new Error('Synthetic source issuer denied');return{uuid:mixedOutcomes?createMessengerPlacementUuid(input.targetTopic,crypto.randomUUID()):crypto.randomUUID(),streamUuid:input.stream,authorUuid:input.author,payload};}
  });
  let nativeGrant:any=null;
  if(input.seedNative) {
   const request={uuid:input.request,streamUuid:input.stream,authorUuid:input.requester,payload:{kind:'key_request' as const,
    keyUuid:input.key,deviceId:input.nativeDevice,deviceName:'Synthetic native device',
    publicKey:{bytes:new Uint8Array(Buffer.from(input.publicKey,'base64'))}}};
   const granted=await issuer.grantKey(request);if(!granted.ok)throw new Error('Synthetic native seed grant failed');
   nativeGrant={uuid:granted.value.uuid,author_uuid:input.author,payload:encodeChatE2eePayload(granted.value.payload)};
  }
  const probe=await provider.encryptMessage({groupKey:key.value,plaintext,context:{streamUuid:input.stream,keyUuid:input.key}});
  if(!probe.ok)throw new Error('Synthetic source probe unavailable');
  const recipientScope={...scope,owner:{...owner,userUuid:input.sourceRecipient},deviceId:input.sourceDevice};
  const storage={
   readChatKey:async()=>recipientKey,readChatKeyDeletionRevision:async()=>0,
   listPendingRequests:async()=>[...pending.values()],
   writePendingRequest:async(_address:any,item:any,options:any)=>{
    if(options?.isCurrent?.()===false||options?.expectedDeletionRevision!==0)return false;
    pending.set(item.localRequestId,item);pairs.push(item);return true;
   },
   bindPendingRequest:async(_address:any,id:string,uuid:string,options:any)=>{
    if(options?.isCurrent?.()===false||!pending.has(id))return false;
    pending.set(id,{...pending.get(id),requestMessageUuid:uuid});return true;
   },
   readRequestCompletion:async(_address:any,id:string)=>completions.get(id)??null,
   readRequestCompletionByMessageUuid:async(_address:any,uuid:string)=>[...completions.values()].find(item=>item.requestMessageUuid===uuid)??null,
   completeGrant:async(_address:any,grant:any,options:any)=>{
    const saved=pending.get(grant.localRequestId);
    if(options?.isCurrent?.()===false||!saved||saved.requestMessageUuid!==grant.requestMessageUuid||completions.has(grant.localRequestId))return false;
    recipientKey={groupKey:{bytes:grant.groupKey.bytes.slice()}};pending.delete(grant.localRequestId);
    completions.set(grant.localRequestId,{localRequestId:grant.localRequestId,requestMessageUuid:grant.requestMessageUuid,
     responseMessageUuid:grant.responseMessageUuid,outcome:'granted'});return true;
   },
   completeReject:async(_address:any,reject:any,options:any)=>{
    const saved=pending.get(reject.localRequestId);
    if(options?.isCurrent?.()===false||!saved||saved.requestMessageUuid!==reject.requestMessageUuid||completions.has(reject.localRequestId))return false;
    pending.delete(reject.localRequestId);completions.set(reject.localRequestId,{...reject,outcome:'rejected'});return true;
   }
  };
  recipient=new ChatE2eeController({scope:recipientScope,device:{deviceId:input.sourceDevice,deviceName:'Synthetic independent recipient'},provider,storage,
   getCurrentScope:()=>recipientScope,getRights:()=>({canRequest:true,canSend:true,canGrant:false,canReject:false,canRotate:false}),
   isIssuerAllowed:(author)=>author===input.requester&&input.nativeIssuerAllowed===true,isAnnouncerAllowed:()=>false,isRequesterAllowed:()=>true,
   sendPayload:async(payload,guard,canonical)=>{
    if(!guard()||payload.kind!=='key_request'||!canonical)throw new Error('Synthetic recipient request denied');
    return{uuid:createMessengerPlacementUuid(input.requestTopic,canonical),streamUuid:input.stream,authorUuid:input.sourceRecipient,payload};
   }
  });
  const loaded=await recipient.loadKey();if(!loaded.ok)throw new Error('Synthetic recipient load failed');
  const requested=await recipient.requestKey();if(!requested.ok||requested.value.payload.kind!=='key_request')throw new Error('Synthetic source recipient request failed');
  const request=requested.value;const saved=[...pending.values()][0];
  if(pending.size!==1||!saved||saved.requestMessageUuid!==request.uuid||recipientKey?.groupKey)throw new Error('Synthetic recipient pending identity invalid');
  let mixedGrant:any=null,mixedReject:any=null;
  if(mixedOutcomes) {
   const granted=await issuer.grantKey(request),rejected=await issuer.rejectKey(request);
   if(!granted.ok||!rejected.ok)throw new Error('Synthetic mixed outcome pair unavailable');
   mixedGrant={uuid:granted.value.uuid,author_uuid:granted.value.authorUuid,payload:encodeChatE2eePayload(granted.value.payload)};
   mixedReject={uuid:rejected.value.uuid,author_uuid:rejected.value.authorUuid,payload:encodeChatE2eePayload(rejected.value.payload)};
  }
  process.stdout.write(JSON.stringify({ok:true,readyForNativeKeyResponse:true,nativeGrant,
   encrypted:{kind:'e2ee',key_uuid:input.key,content:Buffer.from(probe.value.bytes).toString('base64')},
   request:{uuid:request.uuid,author_uuid:request.authorUuid,payload:encodeChatE2eePayload(request.payload)},
   recipientPending:pending.size,recipientHasKey:false,independentOwner:input.sourceRecipient,independentDevice:input.sourceDevice,
   ...(mixedOutcomes?{mixedOutcomePair:true,mixedGrant,mixedReject}:{})})+'\n');
  const second=await iterator.next();if(second.done)throw new Error('Synthetic native response missing');const frame=second.value;
  if(frame.kind==='finish-without-response') {
   if(!(await iterator.next()).done)throw new Error('Synthetic peer extra frame');
   process.stdout.write(JSON.stringify({ok:true,recipientStillPending:pending.size===1,recipientHasKey:recipientKey?.groupKey!=null,
    completionCount:completions.size})+'\n');return;
  }
  const response=frame.response,payload=response?.payload;
  if(frame.kind!=='verify-native-key-response'||!response||response.project_id!==input.project||response.stream_uuid!==input.stream
   ||response.topic_uuid!==input.targetTopic||response.author_uuid!==input.requester
   ||response.uuid!==createMessengerPlacementUuid(input.targetTopic,frame.canonical)
   ||!payload||(payload.kind!=='key_grant'&&payload.kind!=='key_reject')||payload.key_uuid!==input.key
   ||payload.request_message_uuid!==request.uuid||payload.sender_device_id!==input.nativeDevice||payload.recipient_device_id!==input.sourceDevice)
   throw new Error('Synthetic native response identity invalid');
  const decoded=decodeChatE2eePayload(payload);if(!decoded.ok)throw new Error('Synthetic native response codec invalid');
  const ingested=await recipient.ingestKeyMessage({uuid:response.uuid,streamUuid:input.stream,authorUuid:response.author_uuid,payload:decoded.value});
  if(!ingested.ok)throw new Error('Synthetic source recipient rejected native response');
  const completed=completions.get(saved.localRequestId);
  let matchesExpected=false,groupMatchesIssuer=false,nativeProbeVerified=false,matchesNativeProbe=false;
  if(frame.nativeProbe!==undefined) {
   if(input.seedNative||mixedOutcomes||payload.kind!=='key_grant'||!recipientKey?.groupKey)
    throw new Error('Synthetic native public probe context invalid');
   const publicProbe=frame.nativeProbe;
   if(!publicProbe||typeof publicProbe!=='object'||Object.keys(publicProbe).sort().join(',')!=='content,key_uuid,kind'
    ||publicProbe.kind!=='e2ee'||publicProbe.key_uuid!==input.key||typeof publicProbe.content!=='string'
    ||publicProbe.content.length>4096)throw new Error('Synthetic native public probe invalid');
   const decodedProbe=decodeChatE2eePayload(publicProbe);
   if(!decodedProbe.ok||decodedProbe.value.kind!=='e2ee')throw new Error('Synthetic native public probe codec invalid');
   const read=await provider.decryptMessage({groupKey:recipientKey.groupKey,envelope:decodedProbe.value.content,
    context:{streamUuid:input.stream,keyUuid:input.key}});
   if(!read.ok)throw new Error('Synthetic native public probe could not decrypt');decrypted=read.value;
   nativeProbeVerified=true;
   matchesNativeProbe=decrypted.length===plaintext.length&&decrypted.every((byte,index)=>byte===plaintext[index]);
   groupMatchesIssuer=recipientKey.groupKey.bytes.length===key.value.bytes.length&&recipientKey.groupKey.bytes.every((byte:number,index:number)=>byte===key.value.bytes[index]);
  } else
  if(payload.kind==='key_grant') {
   if(!recipientKey?.groupKey)throw new Error('Synthetic recipient key not imported');
   groupMatchesIssuer=recipientKey.groupKey.bytes.length===key.value.bytes.length&&recipientKey.groupKey.bytes.every((byte:number,index:number)=>byte===key.value.bytes[index]);
   const read=await provider.decryptMessage({groupKey:recipientKey.groupKey,envelope:probe.value,context:{streamUuid:input.stream,keyUuid:input.key}});
   if(!read.ok)throw new Error('Synthetic recipient could not decrypt');decrypted=read.value;
   matchesExpected=decrypted.length===plaintext.length&&decrypted.every((byte,index)=>byte===plaintext[index]);
  }
  if(!(await iterator.next()).done)throw new Error('Synthetic peer extra frame');
  process.stdout.write(JSON.stringify({ok:true,completionOutcome:completed?.outcome??null,completionRequest:completed?.requestMessageUuid??null,
   completionResponse:completed?.responseMessageUuid??null,recipientPending:pending.size,recipientHasKey:recipientKey?.groupKey!=null,
   matchesExpected,groupMatchesIssuer,...(frame.nativeProbe!==undefined?{nativeProbeVerified,matchesNativeProbe}:{})})+'\n');
 } finally {
  decrypted?.fill(0);plaintext.fill(0);recipientKey?.groupKey?.bytes.fill(0);key.value.bytes.fill(0);
  for(const item of pairs){item.privateKey.fill(0);item.recovery?.previousGroupKey?.bytes.fill(0);item.recovery?.envelope?.bytes.fill(0);}
  pending.clear();completions.clear();recipient?.invalidate();issuer?.invalidate();
 }
}

if(process.argv.slice(2).includes('--mixed-outcomes-pair')) {
 await verifyNativeKeyResponseMode(true);
} else if(process.argv.slice(2).includes('--verify-native-key-response')) {
 await verifyNativeKeyResponseMode();
} else if(process.argv.slice(2).includes('--verify-native-send')) {
 await verifyNativeSendMode();
} else {
// BEGIN ORIGINAL ONE-SHOT BODY
const input=JSON.parse(fs.readFileSync(0,'utf8'));
const provider=createRealChatCryptoProvider();
const key=await provider.createGroupKey(); if(!key.ok)throw new Error('Synthetic peer key unavailable');
const owner={accountId:'synthetic',instanceId:'synthetic',organizationId:'synthetic',projectId:input.project,userUuid:input.author};
const scope={owner,deviceId:input.senderDevice,streamUuid:input.stream,keyUuid:input.key,runtimeGeneration:1};
const manager=input.role==='owner'||input.role==='administrator';
const controller=new ChatE2eeController({scope,device:{deviceId:scope.deviceId,deviceName:'Synthetic JS issuer'},provider,
 storage:{readChatKey:async()=>({groupKey:key.value}),readChatKeyDeletionRevision:async()=>0},
 getCurrentScope:()=>scope,getRights:()=>({canRequest:true,canSend:true,canGrant:manager,canReject:manager,canRotate:input.role==='owner'}),
 isIssuerAllowed:()=>manager,isAnnouncerAllowed:()=>manager,isRequesterAllowed:()=>true,
 sendPayload:async(payload,guard)=>{if(!guard())throw new Error('Synthetic peer denied');return{uuid:crypto.randomUUID(),streamUuid:input.stream,authorUuid:input.author,payload};}
});
const request={uuid:input.request,streamUuid:input.stream,authorUuid:input.requester,payload:{kind:'key_request',keyUuid:input.key,
 deviceId:input.recipientDevice,deviceName:'Synthetic native device',publicKey:{bytes:new Uint8Array(Buffer.from(input.publicKey,'base64'))}}};
const response=input.reject?await controller.rejectKey(request):await controller.grantKey(request);
let output;
if(!response.ok)output={ok:false,reason:response.reason};
else{
 const encrypted=await provider.encryptMessage({groupKey:key.value,plaintext:new TextEncoder().encode(input.plaintext),context:{streamUuid:input.stream,keyUuid:input.key}});
 if(!encrypted.ok)throw new Error('Synthetic ciphertext unavailable');
 output={ok:true,message:{uuid:response.value.uuid,author_uuid:input.author,payload:encodeChatE2eePayload(response.value.payload)},
  encrypted:{kind:'e2ee',key_uuid:input.key,content:Buffer.from(encrypted.value.bytes).toString('base64')}};
}
key.value.bytes.fill(0);controller.invalidate();
process.stdout.write(JSON.stringify(output));
// END ORIGINAL ONE-SHOT BODY
}
