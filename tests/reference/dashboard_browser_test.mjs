#!/usr/bin/env node
// Real Chromium/CDP smoke and accessibility/layout checks. Fixture metrics are
// explicitly local test data, intercepted only in this isolated browser. HTML
// and navigation are served by the actual C++ binary. No third-party packages.
import assert from 'node:assert/strict';
import {spawn} from 'node:child_process';
import {mkdtemp, mkdir, writeFile, rm} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import path from 'node:path';
import net from 'node:net';
import {setTimeout as delay} from 'node:timers/promises';

const binary=path.resolve(process.argv[2] || 'build/fortis_node_operations_test');
const out=path.resolve(process.argv[3] || 'build/dashboard-evidence');
const chrome=process.env.CHROME || '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome';
await mkdir(out,{recursive:true});
const temporary=await mkdtemp(path.join(tmpdir(),'fortis-dashboard-browser-'));
const kv=path.join(temporary,'kv');await mkdir(kv);
const socket=net.createServer();await new Promise(resolve=>socket.listen(0,'127.0.0.1',resolve));
const port=socket.address().port;await new Promise(resolve=>socket.close(resolve));
const base='http://127.0.0.1:'+port;
const server=spawn(binary,[String(port)],{env:{...process.env,FORTIS_KV_DIR:kv},stdio:['ignore','pipe','pipe']});
let serverLog='';server.stdout.on('data',d=>serverLog+=d);server.stderr.on('data',d=>serverLog+=d);
let browser,ws,clientId=0;const pending=new Map(),events=new Map();
let mode='representative',clockOffset=0,requests=0,externalRequests=[];
const errors=[],checks=[];
const originalNow=Date.now;
function fixtureStatus() {
  const now=originalNow()+clockOffset;
  const status={ok:true,version:'local-fixture',node:'native-cpp',uptimeSec:7200,memory:{rss:48*1048576,heapUsed:null},cpu:{percent:125},disk:{used:2*1073741824,total:10*1073741824},stats:{window5m:{requests:400,errors:4,clients:12}},resources:{sampledAtMs:now,diskSampledAtMs:now,diskScope:'filesystem',diskAvailableBytes:7.5*1073741824,threads:14,openDescriptors:23,peakRssBytes:52*1048576,virtualBytes:512*1048576,cgroupMemoryBytes:72*1048576,cgroupMemoryLimitBytes:256*1048576,cpuQuotaCores:2,readBytesSinceBoot:1048576,writtenBytesSinceBoot:2097152},database:{indexedKeys:10240,fileBytes:40*1048576,liveBytes:24*1048576,compactionsCompleted:8,compactionsFailed:0,sampleAgeSec:1,compactionPhase:'idle',recoveryPending:false,maxLockHoldUs:250},priceSnapshot:{ready:true,ageMs:200,sequence:123,unavailable:0},priceStore:{memoryHits:8200,contractMemoryHits:12},snapshotAuth:{hits:300,misses:10,invalidations:2}};
  status.databaseActivity={sampleMs:requests*1000,boundsUs:[100,1000],reads:[requests*10,requests*2,0],writes:[requests*2,requests,0],fileSync:[requests,0,0]};
  if(mode==='empty'){status.stats.window5m={requests:0,errors:0,clients:0};status.cpu.percent=0;status.memory.rss=0;}
  if(mode==='unsupported'){status.cpu.percent=null;status.memory={rss:null,heapUsed:null};status.disk=null;status.resources={sampledAtMs:now,diskSampledAtMs:now,diskScope:'unavailable'};}
  return status;
}
const aggregate=(service,requests)=>({service,requests,errors5xx:requests?1:0,errors4xx:requests?2:0,rateLimited:requests?1:0,avgMs:15,p50Ms:10,p95Ms:25,p99Ms:50,maxMs:43,avgQueueMs:2,maxQueueMs:4,ownerSamples:0,avgOwnerMs:0,maxOwnerMs:0});
function fixtureStats() {
  const empty=mode==='empty'||mode==='unsupported';
  const routes=empty?[]:Array.from({length:75},(_,i)=>({...aggregate(i<40?'Prices':'Devices',100-i),route:i===0?'/v1/<img src=x onerror=alert(1)>':'/v1/fixture/route-'+i,method:'GET'}));
  return {windowMinutes:15,omittedRouteSamples:mode==='omitted'?7:0,routes,groups:[aggregate('Prices',empty?0:240),aggregate('Devices',empty?0:60),aggregate('AI',0)],platforms:[{os:'unknown',requests:empty?0:100},{os:'iOS',requests:empty?0:200}],admissionSinceBoot:{connectionQueue:0,spotSlots:2,connectionQueueExpired:1}};
}
function fixtureHistory() {
  const now=Math.floor((originalNow()+clockOffset)/60000)*60;
  const range=value=>({min:value,avg:value,max:value,samples:12});
  return {slotDurationSec:60,maxMinutes:480,slots:Array.from({length:480},(_,i)=>{
    const observed=i>=390 && mode!=='unsupported';
    const n=mode==='empty'?0:40+Math.round(20*Math.sin(i/5));
    return {created:now-(479-i)*60,duration:60,observed,memoryMb:observed?range(mode==='empty'?0:45+Math.sin(i/8)*3):null,cpuPercent:observed?range(mode==='empty'?0:90+Math.sin(i/7)*40):null,diskScope:observed?'filesystem':null,diskUsedMb:observed?range(1900+i/4):null,requests:{total:observed?n:0,errors:observed&&i%10===0?1:0,avgDurationMs:observed&&n?12+Math.sin(i/5)*4:0,maxDurationMs:observed&&n?40:0}};
  })};
}
function send(method,params={},sessionId) {
  const id=++clientId;
  return new Promise((resolve,reject)=>{const timer=setTimeout(()=>{pending.delete(id);reject(new Error('CDP timeout '+method));},15000);pending.set(id,{resolve,reject,timer});ws.send(JSON.stringify({id,method,params,...(sessionId?{sessionId}:{})}));});
}
async function until(fn,message,timeout=10000){const start=originalNow();while(originalNow()-start<timeout){if(await fn())return;await delay(40);}throw new Error(message);}
async function stop(child) {
  if(!child||child.exitCode!==null)return;
  child.kill('SIGTERM');await Promise.race([new Promise(resolve=>child.once('exit',resolve)),delay(5000)]);
  if(child.exitCode===null)child.kill('SIGKILL');
}
try {
  await until(async()=>{try{return (await fetch(base+'/healthz')).ok;}catch{return false;}},'C++ server startup failed');
  browser=spawn(chrome,['--headless=new','--no-first-run','--no-default-browser-check','--disable-background-networking','--disable-extensions','--remote-debugging-port=0','--user-data-dir='+path.join(temporary,'chrome'),'about:blank'],{stdio:['ignore','pipe','pipe']});
  let browserLog='';let endpoint;
  browser.stderr.on('data',data=>{browserLog+=data;endpoint=browserLog.match(/DevTools listening on (ws:\/\/\S+)/)?.[1];});
  await until(()=>endpoint,'Chrome CDP startup failed');
  ws=new WebSocket(endpoint);await new Promise((resolve,reject)=>{ws.addEventListener('open',resolve,{once:true});ws.addEventListener('error',reject,{once:true});});
  ws.addEventListener('message',message=>{const data=JSON.parse(message.data);if(data.id){const request=pending.get(data.id);if(!request)return;clearTimeout(request.timer);pending.delete(data.id);data.error?request.reject(new Error(JSON.stringify(data.error))):request.resolve(data.result);}else (events.get(data.method)||[]).forEach(fn=>fn(data.params,data.sessionId));});
  const target=await send('Target.createTarget',{url:'about:blank'});
  const {sessionId}=await send('Target.attachToTarget',{targetId:target.targetId,flatten:true});
  const call=(method,params)=>send(method,params,sessionId);
  const evaluate=async(expression)=>{const result=await call('Runtime.evaluate',{expression,returnByValue:true,awaitPromise:true});if(result.exceptionDetails)throw new Error(JSON.stringify(result.exceptionDetails));return result.result.value;};
  events.set('Runtime.exceptionThrown',[params=>errors.push(params.exceptionDetails.text)]);
  events.set('Fetch.requestPaused',[async params=>{
    try {
      const url=new URL(params.request.url);
      if(url.origin!==base){externalRequests.push(params.request.url);await call('Fetch.failRequest',{requestId:params.requestId,errorReason:'BlockedByClient'});return;}
      if(url.pathname.startsWith('/releasesz')) {
        const current={deploymentId:27,instanceId:29,version:'node-update-fixture',sha256:'a'.repeat(64),sourceCommit:'b'.repeat(40),phase:'stable',outcome:'accepted',reason:'none',processStartedAtMs:originalNow()-3600000,trafficEnabledAtMs:originalNow()-3500000,stableAtMs:originalNow()-3400000,signatureVerified:true,timeUncertain:true};
        let body;
        if(url.pathname==='/releasesz') body=mode==='release-unmanaged'?{schemaVersion:1,managed:false,current:null,candidate:null,latestPage:null,observedAtMs:null}:{schemaVersion:1,managed:true,observedAtMs:originalNow()-(mode==='release-stale'?30000:0),latestPage:1,current,candidate:null};
        else {const page=Number(url.searchParams.get('page'));body={schemaVersion:1,page,previousPage:page===1?0:null,records:Array.from({length:page===1?2:25},(_,i)=>({...current,deploymentId:(page===1?27:25)-i,version:i===0?'<img src=x onerror=alert(1)>':'historical-release',trafficDisabledAtMs:originalNow()-1000}))};}
        await call('Fetch.fulfillRequest',{requestId:params.requestId,responseCode:200,responseHeaders:[{name:'Content-Type',value:'application/json'}],body:Buffer.from(JSON.stringify(body)).toString('base64')});return;
      }
      const key={'/statusz':'status','/statsz':'stats','/status-history':'history','/errorsz':'errors'}[url.pathname];
      if(!key){await call('Fetch.continueRequest',{requestId:params.requestId});return;}
      requests++;
      const body=key==='errors'?{windowMinutes:15,threshold:5,totalErrors:0,totalRequests:4,routes:[],recent:[],operational:{persisted:true,writeFailed:false,records:[
        {origin:'node',service:'storage',code:'write_failed',provider:'',httpStatus:null,count:2,lastSeenMs:Date.now()},
        {origin:'third_party',service:'providers',code:'http_rate_limited',provider:'coingecko',httpStatus:429,count:9,lastSeenMs:Date.now()},
        {origin:'unclassified',service:'prices',code:'quote_unavailable',provider:'<img src=x onerror=alert(1)>',httpStatus:null,count:1,lastSeenMs:Date.now()}
      ]}}:mode==='malformed'?{}:key==='status'?fixtureStatus():key==='stats'?fixtureStats():fixtureHistory();
      await call('Fetch.fulfillRequest',{requestId:params.requestId,responseCode:mode==='error'?503:200,responseHeaders:[{name:'Content-Type',value:'application/json'}],body:Buffer.from(JSON.stringify(body)).toString('base64')});
    }catch(error){errors.push(String(error));}
  }]);
  await call('Page.enable');await call('Runtime.enable');await call('Fetch.enable',{patterns:[{urlPattern:'*'}]});
  await call('Emulation.setDeviceMetricsOverride',{width:1440,height:1100,deviceScaleFactor:1,mobile:false});
  async function homeShot(name) {
    const data=await call('Page.captureScreenshot',{format:'png',captureBeyondViewport:false});
    await writeFile(path.join(out,name+'.png'),Buffer.from(data.data,'base64'));
  }
  await call('Page.navigate',{url:base+'/'});await call('Page.bringToFront');
  await until(()=>evaluate("document.querySelector('.services li')!==null && document.images[0].complete"),'Home page did not render');
  assert.equal(await evaluate("document.querySelectorAll('.services li').length"),8);
  assert.equal(await evaluate("document.querySelectorAll('.services li.on').length"),1);
  assert.equal(await evaluate("document.querySelectorAll('.future li').length"),5);
  assert.equal(await evaluate("document.querySelector('meta[property=\"og:url\"]').content"),'https://node.fortisnet.io/');
  await evaluate("document.fonts.ready");
  await new Promise(resolve=>setTimeout(resolve,200));
  await homeShot('home-desktop');
  await call('Emulation.setDeviceMetricsOverride',{width:390,height:844,deviceScaleFactor:1,mobile:true});
  assert.ok(await evaluate("document.documentElement.scrollWidth <= innerWidth"));
  await homeShot('home-mobile');
  await call('Emulation.setDeviceMetricsOverride',{width:320,height:700,deviceScaleFactor:1,mobile:true});
  assert.ok(await evaluate("document.documentElement.scrollWidth <= innerWidth"));
  await evaluate("document.getElementById('future-title').scrollIntoView()");
  await homeShot('home-future-mobile');
  await call('Emulation.setScriptExecutionDisabled',{value:true});
  await call('Page.reload');
  await until(()=>evaluate("document.querySelectorAll('.future li').length===5"),'No-script home is incomplete');
  assert.ok(await evaluate("document.querySelector('.services').textContent.includes('Not enabled')"));
  await call('Emulation.setScriptExecutionDisabled',{value:false});
  checks.push('Branded home, deployment switches, future catalog, social tags, mobile layout and no-script content');
  await call('Emulation.setDeviceMetricsOverride',{width:1440,height:1100,deviceScaleFactor:1,mobile:false});
  await call('Page.navigate',{url:base+'/releases'});
  await until(()=>evaluate("document.getElementById('hash')?.textContent==='"+'a'.repeat(64)+"'"),'Release identity did not render');
  assert.equal(await evaluate("document.querySelectorAll('#history tr').length"),2);
  assert.equal(await evaluate("document.querySelectorAll('#history img').length"),0);
  assert.equal(await evaluate("document.getElementById('copy').disabled"),false);
  await homeShot('releases-desktop');
  await evaluate("document.getElementById('older').click()");
  await until(()=>evaluate("document.querySelectorAll('#history tr').length===25"),'Stable history cursor did not paginate');
  await evaluate("document.getElementById('latest').click()");
  await until(()=>evaluate("document.querySelectorAll('#history tr').length===2"),'Latest release history did not return');
  mode='release-stale';
  await until(()=>evaluate("document.getElementById('notice').textContent.includes('stale')"),'Stale controller observation was not labelled',8000);
  await call('Emulation.setDeviceMetricsOverride',{width:390,height:844,deviceScaleFactor:1,mobile:true});
  assert.ok(await evaluate("document.documentElement.scrollWidth<=innerWidth"));
  await homeShot('releases-mobile');
  mode='release-unmanaged';
  await until(()=>evaluate("document.getElementById('state').textContent.includes('Unmanaged')"),'Unmanaged release state was not labelled',8000);
  assert.equal(await evaluate("document.getElementById('copy').disabled"),true);
  assert.match(await evaluate("document.getElementById('hash').textContent"),/unknown/);
  checks.push('Release SHA identity, signature/provenance, safe history text, stable pagination, stale/unmanaged states and mobile layout');
  mode='representative';
  await call('Emulation.setDeviceMetricsOverride',{width:1440,height:1100,deviceScaleFactor:1,mobile:false});
  await call('Page.navigate',{url:base+'/dashboard'});await call('Page.bringToFront');
  await until(()=>evaluate("document.getElementById('r-cpu-now')?.textContent==='125%'"),'Representative metrics did not render');
  assert.equal(await evaluate("document.getElementById('error-rate').textContent"),'1%');
  assert.equal(await evaluate("document.querySelectorAll('#release-link').length"),1);
  assert.equal(await evaluate("document.querySelectorAll('#overview-groups tr').length"),2);
  assert.equal(await evaluate("document.querySelectorAll('#detail-routes img').length"),0);
  checks.push('Measured values, >100% CPU, merged group display and safe text rendering');
  async function shot(name){await evaluate("new Promise(r=>requestAnimationFrame(()=>requestAnimationFrame(r)))");const data=await call('Page.captureScreenshot',{format:'png',captureBeyondViewport:false});await writeFile(path.join(out,name+'.png'),Buffer.from(data.data,'base64'));}
  await shot('overview-desktop');
  await evaluate("document.querySelector('[data-minutes=\"480\"]').click()");
  assert.match(await evaluate("document.querySelector('#r-memory-chart .chart-note').textContent"),/^90 measured minute slots/);
  checks.push('Eight-hour selection preserves 390 missing slots as gaps');
  await evaluate("document.querySelector('[data-view=\"traffic\"]').click()");
  await until(()=>evaluate("document.getElementById('page-title').textContent==='Every request tells a story.'"),'Traffic view navigation failed');
  assert.equal(await evaluate("document.querySelectorAll('#detail-routes tr').length"),50);
  await evaluate("document.getElementById('routes-next').click()");
  assert.equal(await evaluate("document.querySelectorAll('#detail-routes tr').length"),25);
  await evaluate("document.getElementById('route-filter').value='route-70';document.getElementById('route-filter').dispatchEvent(new Event('input'))");
  assert.equal(await evaluate("document.querySelectorAll('#detail-routes tr').length"),1);
  await evaluate("document.getElementById('route-filter').value='';document.getElementById('route-filter').dispatchEvent(new Event('input'))");
  await shot('api-performance-desktop');checks.push('History navigation, route filter and bounded 50-row pagination');
  await evaluate("document.querySelector('[data-view=\"resources\"]').click()");await shot('resources-desktop');
  await evaluate("document.querySelector('[data-view=\"guide\"]').click()");assert.match(await evaluate("document.getElementById('content').textContent"),/p95 means/);await shot('guide-desktop');
  await evaluate("document.querySelector('[data-view=\"overview\"]').click()");
  await evaluate("document.getElementById('r-cpu-chart').focus();document.getElementById('r-cpu-chart').dispatchEvent(new KeyboardEvent('keydown',{key:'ArrowLeft',bubbles:true}))");
  assert.equal(await evaluate("document.querySelector('#r-cpu-chart .chart-tip').hidden"),false);checks.push('Keyboard-inspectable charts');
  await call('Emulation.setDeviceMetricsOverride',{width:390,height:844,deviceScaleFactor:1,mobile:true});await delay(150);
  assert.equal(await evaluate('document.documentElement.scrollWidth<=innerWidth'),true);
  await shot('overview-mobile');
  await evaluate("document.querySelector('[data-view=\"traffic\"]').click()");assert.equal(await evaluate('document.documentElement.scrollWidth<=innerWidth'),true);await shot('api-performance-mobile');
  for (const view of ['resources','guide']) { await evaluate("document.querySelector('[data-view=\""+view+"\"]').click()");assert.equal(await evaluate('document.documentElement.scrollWidth<=innerWidth'),true);await shot(view+'-mobile'); }
  checks.push('390px mobile layout without page overflow in all four views');
  await call('Emulation.setDeviceMetricsOverride',{width:1440,height:1100,deviceScaleFactor:1,mobile:false});
  async function changeMode(next,predicate){
    mode=next;clockOffset+=31000;
    await evaluate("(async()=>{window.testRealNow ||= Date.now; Date.now=()=>window.testRealNow()+"+clockOffset+"; const updated=document.getElementById('updated'); const finished=new Promise(resolve=>{const observer=new MutationObserver(()=>{observer.disconnect();resolve();});observer.observe(updated,{childList:true,subtree:true});});const p=document.getElementById('pause');if(p.getAttribute('aria-pressed')!=='true')p.click();p.click();await finished;return true;})()");
    await until(()=>evaluate(predicate),'Mode did not render: '+next);
  }
  await evaluate("document.querySelector('[data-view=\"overview\"]').click()");
  await changeMode('empty',"document.getElementById('error-rate').textContent==='No traffic'");assert.equal(await evaluate("document.getElementById('r-cpu-now').textContent"),'0%');assert.equal(await evaluate("document.getElementById('r-memory-now').textContent"),'0 B');checks.push('Known zero is distinct from no traffic and unavailable measurements');
  await changeMode('unsupported',"document.getElementById('r-cpu-now').textContent==='Not reported'");assert.equal(await evaluate("document.querySelectorAll('#r-cpu-chart svg').length"),0);await shot('unavailable-metrics');
  await changeMode('omitted',"document.getElementById('fetch-notice').textContent.includes('7 route samples')");checks.push('Omission coverage warning');
  await changeMode('error',"document.getElementById('live-state').dataset.state==='stale'");assert.match(await evaluate("document.getElementById('fetch-notice').textContent"),/may be stale/);assert.equal(await evaluate("document.getElementById('r-cpu-now').textContent"),'125%');await shot('stale-data');checks.push('HTTP failure retains last good values and clearly marks them stale');
  await changeMode('malformed',"document.getElementById('fetch-notice').textContent.includes('Could not refresh')");assert.equal(await evaluate("document.getElementById('live-state').dataset.state"),'stale');checks.push('Malformed successful JSON is not shown as live data');
  await evaluate("document.getElementById('pause').click()");const before=requests;await delay(500);assert.equal(requests,before);assert.equal(await evaluate("document.getElementById('connection').textContent"),'Updates paused');checks.push('Pause stops refresh scheduling');
  mode='representative';clockOffset=0;
  await call('Page.navigate',{url:base+'/benchmark'});
  await until(()=>evaluate("document.getElementById('published-run')?.options.length>=6"),'Published benchmark selector did not render');
  await until(()=>evaluate("document.getElementById('live-rate-chart')?.querySelector('div')!==null"),'Live benchmark chart did not render',12000);
  assert.equal(await evaluate("document.documentElement.textContent.includes('historical experiments, not a live capacity promise')"),true);
  await shot('benchmark-desktop');checks.push('Historical benchmark selection and live internal phase-rate chart');
  await call('Page.navigate',{url:base+'/errors'});
  await until(()=>evaluate("document.querySelector('#e-third-party tbody tr')?.textContent.includes('coingecko')"),'Typed provider errors did not render');
  assert.equal(await evaluate("document.querySelectorAll('#e-node tbody tr').length"),1);
  assert.equal(await evaluate("document.querySelectorAll('#e-third-party tbody tr').length"),1);
  assert.equal(await evaluate("document.querySelectorAll('#e-unclassified tbody tr').length"),1);
  assert.match(await evaluate("document.getElementById('e-third-party').textContent"),/429/);
  assert.equal(await evaluate("document.querySelectorAll('#e-unclassified img').length"),0);
  await shot('typed-errors-desktop');
  await call('Emulation.setDeviceMetricsOverride',{width:390,height:844,deviceScaleFactor:1,mobile:true});
  assert.equal(await evaluate('document.documentElement.scrollWidth<=innerWidth'),true);
  await shot('typed-errors-mobile');
  await call('Emulation.setDeviceMetricsOverride',{width:1440,height:1100,deviceScaleFactor:1,mobile:false});
  checks.push('Node/provider/unclassified panels, actual provider status and escaped diagnostic text');
  await call('Emulation.setScriptExecutionDisabled',{value:true});
  await call('Page.navigate',{url:base+'/dashboard#guide'});
  await until(()=>evaluate("document.getElementById('guide') && getComputedStyle(document.getElementById('guide')).display!=='none'"),'No-script guide is hidden');
  checks.push('No-script explanations and raw aggregate links remain readable');
  assert.deepEqual(errors,[]);assert.deepEqual(externalRequests,[]);
  await writeFile(path.join(out,'browser-result.json'),JSON.stringify({passed:true,checks,externalRequests,consoleErrors:errors,screenshots:'Representative local fixtures only; no production metrics were changed.'},null,2)+'\n');
  await writeFile(path.join(out,'browser.log'),browserLog);
  console.log('Dashboard browser checks passed: '+checks.length+' groups; screenshots in '+out);
} finally {
  if(ws)ws.close();for(const request of pending.values())clearTimeout(request.timer);
  await stop(browser);await stop(server);await writeFile(path.join(out,'browser-server.log'),serverLog);await rm(temporary,{recursive:true,force:true});
}
