/* A late HTTP fallback must not undo a more recent pushed call state. */
'use strict';
const assert=require('node:assert/strict');
const fs=require('node:fs');
const vm=require('node:vm');
const hooks={},received=[];
let source,complete,requests=0,opened=0,closed=0;
const context={
    console,Set,WeakSet,performance,AbortController,setTimeout,clearTimeout,
    blank:false,last:{screen:{touch:true}},location:{hostname:'127.0.0.1'},
    appOpen:null,idleTimer:null,
    document:{body:{classList:{add(){},remove(){}}}},
    T551MPower:{isOpen:()=>false},
    setBlank:value=>{context.blank=value;},applyTouch(){},resetIdle(){},
    openApp:app=>{opened++;context.appOpen=app;},
    closeApp:()=>{closed++;context.appOpen=null;},
    addEventListener:(name,handler)=>{hooks[name]=handler;},
    EventSource:class {constructor(){source=this;}close(){this.closed=true;}},
    fetch:()=>{requests++;return new Promise(resolve=>{complete=resolve;});}
};
context.window=context;vm.createContext(context);
vm.runInContext(fs.readFileSync(__dirname+'/../../root/usr/share/e5-infoscreen/www/calls.js','utf8'),context);
const calls=context.T551MCalls;
const emit=data=>source.onmessage({data:JSON.stringify(data)});
(async()=>{
    const unsubscribe=calls.subscribe(data=>received.push(data));
    calls.boot();emit({calls:[]});assert.equal(requests,0);
    source.onerror();assert.equal(requests,1);
    emit({calls:[{id:'test',state:'active'}]});
    complete({ok:true,json:async()=>({calls:[]})});
    await new Promise(resolve=>setImmediate(resolve));
    assert.equal(received.at(-1).calls[0].id,'test');
    emit({phone_installed:true,calls:[{id:'incoming',state:'ringing-in'}]});
    assert.equal(opened,1);assert.equal(calls.active(),true);
    emit({phone_installed:false,error:'observer unavailable',calls:[{id:'incoming',state:'ringing-in'}]});
    assert.equal(closed,1);assert.equal(calls.active(),false);
    context.blank=true;
    emit({phone_installed:false,calls:[{id:'incoming',state:'ringing-in'}]});
    assert.equal(opened,1);assert.equal(context.blank,true);
    emit({phone_installed:true,calls:[{id:'incoming',state:'ringing-in'}]});
    assert.equal(opened,2);assert.equal(context.blank,false);
    source.onerror();assert.equal(requests,2);
    complete({ok:false,status:404,json:async()=>({error:'not installed'})});
    await new Promise(resolve=>setImmediate(resolve));
    assert.equal(calls.active(),false);assert.equal(closed,2);
    assert.equal(received.at(-1).phone_installed,false);
    const count=received.length;unsubscribe();emit({calls:[]});
    assert.equal(received.length,count);
    hooks.pagehide();assert.equal(source.closed,true);
    console.log('Push recovery rejects stale fallback; subscriber and page cleanup PASS');
})().catch(error=>{hooks.pagehide();console.error(error);process.exitCode=1;});
