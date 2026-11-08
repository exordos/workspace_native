// Root-only future fixture. No external endpoint; no product code or fake QNetworkReply.
import tls from 'node:tls';
import http2 from 'node:http2';
import fs from 'node:fs';
import crypto from 'node:crypto';
const args = Object.fromEntries(process.argv.slice(2).map(s => { const i=s.indexOf('='); return [s.slice(0,i),s.slice(i+1)]; }));
for (const name of ['key','cert','body','descriptor','trace']) if (!args[name]) throw Error('Explicit fixture argument required: '+name);
if (fs.existsSync(args.descriptor)||fs.existsSync(args.trace)) throw Error('Fresh fixture outputs required');
const body=fs.readFileSync(args.body),digest=crypto.createHash('sha256').update(body).digest('hex');
let connectionCount=0,requestCount=0,stopping=false;
const sockets=new Set(),events=[];
const record = value => { events.push(value); fs.appendFileSync(args.trace,JSON.stringify({...value,atNs:process.hrtime.bigint().toString()})+'\n',{mode:0o600}); };
const server=tls.createServer({key:fs.readFileSync(args.key),cert:fs.readFileSync(args.cert),ALPNProtocols:['h2'],requestCert:false},socket=>{
    const connection=++connectionCount; sockets.add(socket);
    record({event:'tls-established',connection,alpn:socket.alpnProtocol,remoteIsLoopback:socket.remoteAddress==='127.0.0.1'});
    socket.on('error',error=>record({event:'socket-error',connection,code:error.code||'unknown'}));
    socket.on('close',()=>{sockets.delete(socket);record({event:'socket-close',connection});});
    if(socket.alpnProtocol!=='h2') {socket.destroy();return;}
    // Keep the actual TLSSocket received from our own TLS server. session.socket
    // is a protective proxy; session.destroy() also sends GOAWAY and is a different fault.
    const session=http2.performServerHandshake(socket);
    session.on('error',error=>record({event:'session-error',connection,code:error.code||'unknown'}));
    session.on('stream',(stream,headers)=>{
        const ordinal=++requestCount;
        const safe={event:'request-headers',ordinal,connection,streamId:stream.id,method:headers[':method'],path:headers[':path'],
            noAuthorization:headers.authorization===undefined,noCookie:headers.cookie===undefined};
        record(safe);
        stream.on('error',error=>record({event:'stream-error',connection,streamId:stream.id,code:error.code||'unknown'}));
        stream.on('close',()=>record({event:'stream-close',connection,streamId:stream.id}));
        if(headers[':method']!=='GET'||headers[':path']!=='/releases/latest'||!safe.noAuthorization||!safe.noCookie)
            {record({event:'invalid-request'});socket.destroy();return;}
        if(ordinal===2) {
            record({event:'controlled-raw-tls-close',ordinal,connection,streamId:stream.id,responseHeadersWritten:false,responseBytes:0,
                ownGoawayCalled:false,ownSessionDestroyCalled:false});
            socket.destroy(); // Deliberate real transport close before any response headers/END_STREAM.
            return;
        }
        if(ordinal!==1&&ordinal!==3) {record({event:'unexpected-extra-request',ordinal});socket.destroy();return;}
        stream.respond({':status':200,'content-type':'application/json','content-length':String(body.length),'x-fixture-reply':String(ordinal)});
        stream.end(body);
        record({event:'response-enqueued',ordinal,connection,streamId:stream.id,bytes:body.length,sha256:digest});
    });
});
server.on('error',error=>{record({event:'server-error',code:error.code||'unknown'});process.exitCode=2;});
server.listen({host:'127.0.0.1',port:0},()=>{
    const address=server.address();
    fs.writeFileSync(args.descriptor+'.own-tmp',JSON.stringify({pid:process.pid,host:'127.0.0.1',port:address.port,
        url:`https://127.0.0.1:${address.port}/releases/latest`,bodyBytes:body.length,bodySha256:digest,node:process.version})+'\n',{mode:0o600});
    fs.renameSync(args.descriptor+'.own-tmp',args.descriptor);
    record({event:'listening',port:address.port});
});
function stop(signal) {
    if(stopping)return;stopping=true;record({event:'owned-cleanup',signal,connections:connectionCount,requests:requestCount});
    server.close(()=>process.exit(0)); for(const socket of sockets)socket.destroy();
}
process.on('SIGTERM',()=>stop('SIGTERM'));process.on('SIGINT',()=>stop('SIGINT'));
