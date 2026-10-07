'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const Core = require('../static/assets/player-core.js');

test('计量：2倍速60秒内容仅记30秒实际收听', () => {
  const m = new Core.ListeningMeter();
  for (let second=0;second<=30;second++) m.sample(second*1000,second*2,true,2);
  assert.equal(m.total,30000);
});
test('计量：暂停、缓冲、静音均不计时，恢复只记实际推进', () => {
  const m = new Core.ListeningMeter();
  m.sample(0,0,true);m.sample(1000,1,true);
  m.sample(2000,1,false);m.sample(32000,1,false);
  m.sample(33000,1,true);m.sample(34000,2,true);
  assert.equal(m.total,2000);
});
test('计量：拖到尾部、回拖和寻址不累计收听', () => {
  const m = new Core.ListeningMeter();m.sample(0,0,true);m.sample(1000,1,true);
  m.sample(1100,599,true,1,true);m.sample(1200,599,true);m.sample(2200,600,true);
  m.breakContinuity();m.sample(2300,20,true);m.sample(3300,21,true);
  assert.equal(m.total,3000);
});
test('计量：后台定时器变慢，仍按音频真实推进计时；卡缓冲仅有1秒推进记1秒', () => {
  const m = new Core.ListeningMeter();m.sample(0,0,true);m.sample(30000,30,true);m.sample(60000,31,true);
  assert.equal(m.total,31000);
});
test('计量：没有seek事件的异常跳跃也不能虚增听时长', () => {
  const m = new Core.ListeningMeter();m.sample(0,0,true);m.sample(500,400,true);m.sample(1000,400,true);
  assert.equal(m.total,0);
});
test('同步：失败保留原sequence，重试不产生第二份累计时长', async () => {
  let connected=false;const calls=[];const snapshots=[];
  const q=new Core.EventOutbox({send:async e=>{calls.push(e);if(!connected)throw new Error('offline');return{accepted:true};},save:events=>{snapshots.push(events);return true;}});
  const event={session_id:'session-one',seq:1,position_ms:1000,listened_ms:1000,state:'playing',rate:1};q.add(event);await q.drain();assert.equal(q.events.length,1);
  connected=true;await q.drain();assert.equal(q.events.length,0);assert.deepEqual(calls[0],calls[1]);assert.equal(snapshots.at(-1).length,0);
});
test('同步：上报中新增暂停事件也会发送，旧端失败不会阻挡另一会话', async () => {
  const sent=[];let q;
  q=new Core.EventOutbox({send:async e=>{sent.push(`${e.session_id}:${e.seq}`);if(e.session_id==='old-session')throw new Error('offline');if(e.seq===1)q.add({session_id:'new-session',seq:2,position_ms:1500,listened_ms:1500,state:'paused'});return{accepted:true};}});
  q.add({session_id:'old-session',seq:1,position_ms:1000,listened_ms:1000,state:'paused'});
  q.add({session_id:'new-session',seq:1,position_ms:1000,listened_ms:1000,state:'playing'});
  await q.drain();assert.deepEqual(sent,['old-session:1','new-session:1','new-session:2']);assert.equal(q.events.length,1);
});
test('同步：事件快照不随后续seek或原对象修改变动，旧会话应答交给中央裁决', async () => {
  const responses=[];let release;
  const q=new Core.EventOutbox({send:e=>new Promise(resolve=>release=()=>resolve({accepted:true,stale:true,progress:{position_ms:9000}})),received:(r,e)=>responses.push([r,e])});
  const e={session_id:'stable-session',seq:1,position_ms:1000,listened_ms:1000,state:'paused'};q.add(e);e.position_ms=8000;const run=q.drain();release();await run;
  assert.equal(responses[0][1].position_ms,1000);assert.equal(responses[0][0].stale,true);
});
test('同步：长期离线队列恢复不能悄悄截断旧记录', () => {
  const events=Array.from({length:300},(_,i)=>({session_id:'session-offline',seq:i+1,position_ms:i*1000,listened_ms:i*1000,state:'playing'}));
  const q=new Core.EventOutbox({events,send:async()=>({accepted:true})});assert.equal(q.events.length,300);
});

function harness({prepare=false,eventFailure=false,terminalAckFailure=false,terminalFailure=false,legacy=false,emptyCentral=false,importSkip=false,catalogueEmpty=false,twoShows=false}={}) {
  const html=fs.readFileSync(path.join(__dirname,'../static/index.html'),'utf8');
  const ids=new Map(),docEvents={},winEvents={},timers=[],requests=[],storage=new Map();let now=0,sessionCount=0;
  const initial=emptyCentral?{status:'unplayed',position_ms:0,listened_ms:0,play_count:0,revision:0}:{status:'in_progress',position_ms:50000,listened_ms:50000,play_count:1,revision:1};
  const progress={e1:{...initial},e2:{status:'unplayed',position_ms:0,listened_ms:0,play_count:0,revision:0}};
  const controls={prepare,eventFailure,terminalAckFailure,terminalFailure,centralPosition:emptyCentral?0:50,importSkip,catalogueEmpty,catalogueOffline:false,catalogueStatus:200,episodesStatus:200,holdCatalogue:false,releaseCatalogue:null};
  const acceptedEvents=new Map(),closedSessions=new Set(),serverListened=new Map(),beacons=[];
  if(legacy){const record={sid:'show',eid:'e1',showName:'中文节目',title:'中文单集一',position:77,duration:600,finished:false,updated:Date.now()-1000};storage.set('portable-podcast-progress-v1',JSON.stringify({'show/e1':record}));storage.set('portable-podcast-last-v1',JSON.stringify(record));}
  class Element {
    constructor(id){this.id=id;this.dataset={};this.hidden=false;this.disabled=false;this.value='';this.attributes={};this.handlers={};this.style={setProperty(){}};this._html='';this.textContent='';this.checked=id==='autoplay';}
    set innerHTML(value){this._html=value;for(const found of String(value).matchAll(/id="([^"]+)"/g))if(!ids.has(found[1]))ids.set(found[1],new Element(found[1]));}
    get innerHTML(){return this._html;}
    setAttribute(name,value){this.attributes[name]=String(value);}getAttribute(name){return this.attributes[name]??null;}removeAttribute(name){delete this.attributes[name];if(name==='src')this._src='';}
    addEventListener(name,fn){(this.handlers[name] ||= []).push(fn);}emit(name){for(const fn of this.handlers[name]||[])fn({target:this});}
    focus(){}contains(other){return other===this;}scrollBy(){}scrollIntoView(){}
    get selectedOptions(){return[{textContent:this.id==='speed'?`${this.value} 倍`:'不定时'}];}
    requestSubmit(){return this.onsubmit({preventDefault(){}});}
  }
  for(const found of html.matchAll(/<[^>]*\bid="([^"]+)"[^>]*>/g)){
    const element=new Element(found[1]);element.hidden=/\shidden(?:\s|>|=)/.test(found[0]);ids.set(found[1],element);
  }
  const a=ids.get('audio');a.paused=true;a.currentTime=0;a.duration=600;a.readyState=4;a.playbackRate=1;a.volume=.7;a.muted=false;a.seeking=false;
  Object.defineProperty(a,'src',{get(){return a._src||'';},set(value){a._src=value;a.attributes.src=value;}});
  a.pause=()=>{if(!a.paused){a.paused=true;a.emit('pause');}};a.load=()=>{if(a.src)queueMicrotask(()=>{a.emit('loadedmetadata');});};a.play=async()=>{a.paused=false;a.emit('playing');};
  ids.get('speed').value='1';ids.get('sleep').value='0';ids.get('theme').value='auto';ids.get('episode-order').value='newest';ids.get('volume').value='70';
  const nav=['library','sources','stats'].map(name=>{const e=new Element();e.dataset.view=name;return e;});
  const filters=['all','unplayed','in_progress','completed'].map(name=>{const e=new Element();e.dataset.filter=name;return e;});
  const document={getElementById:id=>ids.get(id)||null,documentElement:{dataset:{}},activeElement:null,hidden:false,addEventListener:(name,fn)=>(docEvents[name] ||= []).push(fn),querySelectorAll:selector=>selector==='[data-view]'?nav:selector==='[data-filter]'?filters:[]};
  const location={hash:'#library'};
  const detail=eid=>({id:eid,title:eid==='e1'?'中文单集一':'中文单集二',duration:600,ready:true,browser_audio_ready:!controls.prepare,browser_audio_url:controls.prepare?null:`/audio/show/${eid}.mp3`,browser_audio_status:controls.prepare?'preparing':'ready',segments:[{duration_ms:600000}],newer:eid==='e1'?null:{id:'e1',title:'中文单集一',duration:600},older:eid==='e1'?{id:'e2',title:'中文单集二',duration:600}:null,progress:progress[eid]});
  const fetch=async(url,options={})=>{
    const body=options.body?JSON.parse(options.body):null;requests.push({url,body,method:options.method||'GET'});let data;
    if(url==='/api/shows'){
      if(controls.catalogueOffline)throw new Error('offline');
      if(controls.catalogueStatus!==200)return{ok:false,status:controls.catalogueStatus,json:async()=>({error:'请先登录后台'})};
      if(controls.holdCatalogue)await new Promise(resolve=>controls.releaseCatalogue=resolve);
      data={shows:[{id:'show',name:'中文节目',author:'作者',episodes:controls.catalogueEmpty?0:2,art:'/art/show'},...(twoShows?[{id:'second',name:'另一个节目',episodes:controls.catalogueEmpty?0:2,art:'/art/second'}]:[])]};
    }
    else if(/^\/api\/shows\/(?:show|second)\/episodes/.test(url)){if(controls.episodesStatus!==200)return{ok:false,status:controls.episodesStatus,json:async()=>({error:'请先登录后台'})};if(controls.catalogueEmpty)return{ok:false,status:404,json:async()=>({error:'该节目尚未抓取，请点刷新'})};const params=new URL('http://test'+url).searchParams;let list=['e1','e2'].map(eid=>detail(eid));if(params.has('status'))list=list.filter(e=>e.progress.status===params.get('status'));if(params.get('order')==='oldest')list.reverse();data={episodes:list,offset:0,total:list.length};}
    else if(url.startsWith('/api/episodes/'))data=detail(url.split('/').at(-1));
    else if(url==='/api/listening/recent?limit=20')data={episodes:progress.e1.revision?[{show_id:'show',episode_id:'e1',show_name:'中文节目',title:'中文单集一',duration:600,progress:progress.e1}]:[]};
    else if(url==='/api/listening/stats')data={summary:{listened_ms:50000,played_episodes:1,completed_episodes:0,play_count:1},shows:[{show_id:'show',name:'中文节目',listened_ms:50000,played_episodes:1,completed_episodes:0,play_count:1}],days:[],timezone:'Asia/Shanghai'};
    else if(url==='/api/listening/sessions')data={session_id:'session-'+(++sessionCount),position_ms:body.restart?0:controls.centralPosition*1000,next_seq:1,progress:progress[body.episode_id]};
    else if(url==='/api/listening/import'){const entry=body.episodes[0];if(controls.importSkip){progress.e1={...progress.e1,status:'in_progress',position_ms:90000,revision:10};controls.centralPosition=90;}else{progress.e1={...progress.e1,status:entry.completed?'completed':'in_progress',position_ms:entry.position_ms,revision:1,legacy:true};controls.centralPosition=entry.position_ms/1000;}data={imported:controls.importSkip?0:1,skipped:controls.importSkip?1:0,results:[{show_id:entry.show_id,episode_id:entry.episode_id,status:controls.importSkip?'skipped_existing':'imported',progress:progress.e1}]};}
    else if(url==='/api/listening/events'){
      const terminal=['ended','stopped'].includes(body.state),identity=`${body.session_id}:${body.seq}`,previous=acceptedEvents.get(identity);
      if(controls.eventFailure||(terminal&&controls.terminalFailure))throw new Error('offline');
      if(previous){
        if(JSON.stringify(previous.body)!==JSON.stringify(body))return{ok:false,status:409,json:async()=>({error:'同一序号不能对应不同播放记录'})};
        data={...previous.reply,duplicate:true};
      }else{
        if(closedSessions.has(body.session_id))return{ok:false,status:409,json:async()=>({error:'播放会话已结束，请建立新会话'})};
        data={accepted:true,duplicate:false,progress:{...progress.e1,position_ms:body.position_ms,revision:2}};
        acceptedEvents.set(identity,{body:{...body},reply:data});serverListened.set(body.session_id,body.listened_ms);
        if(terminal)closedSessions.add(body.session_id);
      }
      // Simulate the server committing the record but the acknowledgement getting lost.
      if(terminal&&controls.terminalAckFailure)throw new Error('acknowledgement lost');
    }
    else if(url==='/api/prepare')data={ok:true};
    else if(url==='/api/listening/mark')data={progress:{...progress[body.episode_id],status:body.status,revision:10}};
    else if(url==='/api/sources/preview')data={preview_id:'preview-one',source_type:'apple',feed_url:'https://example.org/podcast.xml',show:{name:'新节目',author:'新作者',art:'/art/show',episodes:3,latest:{title:'新单集',duration:900,published:'2026-10-05'}},duplicate:false,warning:'公开免费内容，历史可能不完整'};
    else if(url==='/api/sources')data={show_id:'show',duplicate:false};
    else if(url.startsWith('/api/sources/search?'))data={provider:'apple',provider_name:'苹果播客公开目录',results:[{name:'搜索节目',author:'作者',url:'https://podcasts.apple.com/cn/podcast/test/id123',episodes:20}]};
    else throw new Error('Unexpected API '+url);
    return{ok:true,status:200,json:async()=>data};
  };
  const WallDate=class extends Date{constructor(...args){super(...(args.length?args:[Date.now()+now]));}static now(){return Date.now()+now;}};
  const context={PodcastCore:Core,document,location,history:{replaceState(a,b,hash){location.hash=hash;}},window:{addEventListener:(name,fn)=>(winEvents[name] ||= []).push(fn)},localStorage:{getItem:name=>storage.get(name)??null,setItem:(name,value)=>storage.set(name,value)},crypto:{randomUUID:()=>`request-${requests.length}-${sessionCount}`},performance:{now:()=>now},Date:WallDate,Intl,URL,AbortController,Blob,navigator:{sendBeacon(url,body){beacons.push({url,body});return true;}},matchMedia:()=>({matches:true}),fetch,setTimeout:(fn,delay)=>{const handle={fn,delay};timers.push(handle);return handle;},clearTimeout:handle=>{const at=timers.indexOf(handle);if(at>=0)timers.splice(at,1);},setInterval:(fn,delay)=>{timers.push({fn,delay,interval:true});return 1;},console};
  vm.runInNewContext(fs.readFileSync(path.join(__dirname,'../static/assets/player.js'),'utf8'),context,{filename:'player.js'});
  const flush=async()=>{for(let i=0;i<12;i++)await new Promise(resolve=>setImmediate(resolve));};
  return{ids,a,requests,storage,controls,progress,acceptedEvents,serverListened,beacons,document,flush,timers,winEvents,docEvents,advance(ms,media){now+=ms;a.currentTime+=media;a.emit('timeupdate');},fire(name){for(const fn of winEvents[name]||[])fn({});},target(dataset,selector){return{closest:s=>s===selector?{dataset}:null};}};
}

test('网页：启动从后台恢复，最近收听/单集状态/每节目统计同时可见', async()=>{
  const h=harness();await h.flush();assert.equal(h.ids.get('player').dataset.state,'restored');assert.match(h.ids.get('recent-list').innerHTML,/继续收听/);assert.match(h.ids.get('episode-list').innerHTML,/听了一部分/);assert.match(h.ids.get('episode-list').innerHTML,/未听过/);assert.match(h.ids.get('show-listening-summary').innerHTML,/听过/);assert.equal(h.requests.some(r=>r.url==='/api/recommendations'),false);
});
test('网页：采用整集音频，从共享位置续播，暂停后另一端位置优先', async()=>{
  const h=harness();await h.flush();await h.ids.get('toggle-play').onclick();await h.flush();assert.equal(h.a.src,'/audio/show/e1.mp3');assert.equal(h.a.currentTime,50);h.advance(1000,1);h.advance(1000,1);await h.ids.get('toggle-play').onclick();await h.flush();assert.equal(h.ids.get('player').dataset.state,'paused');h.controls.centralPosition=90;await h.ids.get('toggle-play').onclick();await h.flush();assert.equal(h.a.currentTime,90);assert.equal(h.requests.filter(r=>r.url==='/api/listening/sessions').at(-1).body.position_ms,undefined);
});
test('网页：等待准备可取消自动开播，PCM就绪不能误用分段播放', async()=>{
  const h=harness({prepare:true});await h.flush();await h.ids.get('toggle-play').onclick();await h.flush();assert.equal(h.ids.get('player').dataset.state,'preparing');assert.equal(h.a.src,'');await h.ids.get('toggle-play').onclick();h.controls.prepare=false;const poll=h.timers.find(t=>t.delay===2500);await poll.fn();await h.flush();assert.equal(h.ids.get('player').dataset.state,'paused');assert.equal(h.a.paused,true);assert.equal(h.a.src,'/audio/show/e1.mp3');
});
test('网页：倍速事件报告旧rate后才换速，seek先flush再只改变位置', async()=>{
  const h=harness();await h.flush();await h.ids.get('toggle-play').onclick();await h.flush();h.advance(1000,1);h.advance(1000,1);h.ids.get('speed').value='2';h.ids.get('speed').onchange();await h.flush();assert.equal(h.requests.filter(r=>r.url==='/api/listening/events').at(-1).body.rate,1);h.advance(1000,2);h.advance(1000,2);h.ids.get('seek').value='590';h.ids.get('seek').onchange();await h.flush();const events=h.requests.filter(r=>r.url==='/api/listening/events').map(r=>r.body);const seek=events.at(-1);assert.equal(seek.seek,true);assert.equal(seek.position_ms,590000);assert.equal(seek.listened_ms,events.at(-2).listened_ms);assert.equal(seek.rate,2);assert.ok(seek.listened_ms<=4000);
});
test('网页：同步失败可见，恢复后保持同一个事件重试', async()=>{
  const h=harness({eventFailure:true});await h.flush();await h.ids.get('toggle-play').onclick();await h.flush();assert.equal(h.ids.get('sync-banner').hidden,false);assert.match(h.ids.get('sync-message').textContent,/暂时没有同步/);const first=h.requests.find(r=>r.url==='/api/listening/events');h.controls.eventFailure=false;await h.ids.get('retry-sync').onclick();await h.flush();const retry=h.requests.filter(r=>r.url==='/api/listening/events').findLast(r=>r.body.seq===first.body.seq&&r.body.session_id===first.body.session_id);assert.deepEqual(retry.body,first.body);assert.equal(JSON.parse(h.storage.get('portable-podcast-outbox-v2')).length,0);
});
test('网页：添加节目先预览再确认，公开页警告与Apple目录搜索来源可见', async()=>{
  const h=harness();await h.flush();h.ids.get('source-url').value='https://podcasts.apple.com/cn/podcast/test/id123';await h.ids.get('source-form').onsubmit({preventDefault(){}});await h.flush();assert.match(h.ids.get('source-preview').innerHTML,/新节目/);assert.match(h.ids.get('source-preview').innerHTML,/历史可能不完整/);assert.equal(h.requests.some(r=>r.url==='/api/sources'),false);await h.ids.get('confirm-source').onclick();await h.flush();assert.equal(h.requests.find(r=>r.url==='/api/sources').body.preview_id,'preview-one');h.ids.get('source-search').value='搜索节目';await h.ids.get('source-search-form').onsubmit({preventDefault(){}});await h.flush();assert.match(h.ids.get('source-search-feedback').textContent,/苹果播客公开目录/);assert.match(h.ids.get('source-search-results').innerHTML,/搜索节目/);
});

test('网页：旧浏览器进度可见，中央为空时首次续播安全导入，不补造听时长', async()=>{
  const h=harness({legacy:true,emptyCentral:true});await h.flush();const original=h.storage.get('portable-podcast-progress-v1');assert.equal(h.ids.get('migration-banner').hidden,false);assert.match(h.ids.get('recent-list').innerHTML,/历史进度/);
  await h.ids.get('show-play').onclick();await h.flush();const imported=h.requests.find(r=>r.url==='/api/listening/import');assert.equal(imported.body.episodes[0].position_ms,77000);assert.equal(h.a.currentTime,77);assert.equal(h.progress.e1.listened_ms,0);assert.equal(h.progress.e1.play_count,0);assert.equal(h.storage.get('portable-podcast-progress-v1'),original);assert.equal(h.ids.get('migration-banner').hidden,true);
});
test('网页：导入期间另一端抢先保存，中央已有进度优先', async()=>{
  const h=harness({legacy:true,emptyCentral:true,importSkip:true});await h.flush();await h.ids.get('show-play').onclick();await h.flush();assert.equal(h.a.currentTime,90);assert.equal(h.requests.filter(r=>r.url==='/api/listening/sessions').at(-1).body.position_ms,undefined);
});
test('网页：旧本地断点不会覆盖已有中央进度，原始本地数据仍保留', async()=>{
  const h=harness({legacy:true});await h.flush();await h.ids.get('show-play').onclick();await h.flush();assert.equal(h.a.currentTime,50);assert.equal(h.requests.some(r=>r.url==='/api/listening/import'),false);assert.ok(h.storage.has('portable-podcast-last-v1'));
});
test('网页：音频准备期间睡眠到点，准备完成也保持暂停', async()=>{
  const h=harness({prepare:true});await h.flush();await h.ids.get('toggle-play').onclick();await h.flush();h.ids.get('sleep').value='15';h.ids.get('sleep').onchange();h.advance(15*60*1000+1,0);h.timers.find(t=>t.interval&&t.delay===500).fn();h.controls.prepare=false;await h.timers.find(t=>t.delay===2500).fn();await h.flush();assert.equal(h.a.paused,true);assert.equal(h.ids.get('player').dataset.state,'paused');assert.equal(h.ids.get('sleep').value,'0');
});
test('网页：连播使用开始收听时的顺序，浏览排序变化不反转播放，末集不跨节目', async()=>{
  const h=harness();await h.flush();h.ids.get('episode-order').value='oldest';h.ids.get('episode-order').onchange();await h.flush();await h.ids.get('show-play').onclick();await h.flush();assert.equal(h.a.src,'/audio/show/e2.mp3');h.ids.get('episode-order').value='newest';h.ids.get('episode-order').onchange();await h.flush();h.a.currentTime=600;h.a.paused=true;h.a.emit('ended');await h.flush();assert.equal(h.a.src,'/audio/show/e1.mp3');const count=h.requests.filter(r=>r.url==='/api/listening/sessions').length;h.a.currentTime=600;h.a.paused=true;h.a.emit('ended');await h.flush();assert.equal(h.requests.filter(r=>r.url==='/api/listening/sessions').length,count);
});
test('网页：筛选走完整后台分页，手工标记不提供虚构听时长', async()=>{
  const h=harness();await h.flush();h.ids.get('episode-filters').onclick({target:h.target({filter:'unplayed'},'[data-filter]')});await h.flush();const page=h.requests.filter(r=>r.url.includes('/episodes?')).at(-1);assert.match(page.url,/web=1/);assert.match(page.url,/status=unplayed/);assert.match(h.ids.get('episode-list').innerHTML,/中文单集二/);assert.doesNotMatch(h.ids.get('episode-list').innerHTML,/中文单集一/);
  const mark={dataset:{markEpisode:'e2',mark:'completed'},disabled:false};h.ids.get('episode-list').onclick({target:{closest:s=>s==='[data-mark-episode]'?mark:null}});await h.flush();const body=h.requests.find(r=>r.url==='/api/listening/mark').body;assert.equal(body.status,'completed');assert.equal(body.listened_ms,undefined);
});
test('网页：粘贴带说明的分享文字可提取地址，非法协议不会交给后台', async()=>{
  const h=harness();await h.flush();h.ids.get('source-url').value='收听「商业就是这样」 https://podcasts.apple.com/cn/podcast/id1552904790';await h.ids.get('source-form').onsubmit({preventDefault(){}});await h.flush();assert.equal(h.requests.find(r=>r.url==='/api/sources/preview').body.url,'https://podcasts.apple.com/cn/podcast/id1552904790');const n=h.requests.length;h.ids.get('source-url').value='javascript:alert(1)';await h.ids.get('source-form').onsubmit({preventDefault(){}});await h.flush();assert.equal(h.requests.length,n);assert.match(h.ids.get('source-feedback').textContent,/节目地址/);
});


test('网页：网络音频失败时一次点击就重试，不先误执行暂停', async()=>{
  const h=harness();await h.flush();await h.ids.get('toggle-play').onclick();await h.flush();h.a.paused=false;h.a.emit('error');assert.equal(h.ids.get('player').dataset.state,'error');const before=h.requests.filter(r=>r.url==='/api/listening/sessions').length;await h.ids.get('toggle-play').onclick();await h.flush();assert.equal(h.requests.filter(r=>r.url==='/api/listening/sessions').length,before+1);assert.equal(h.ids.get('player').dataset.state,'playing');
});

function sessionEvents(h,sessionId){return h.requests.filter(r=>r.url==='/api/listening/events'&&r.body.session_id===sessionId).map(r=>r.body);}
function pendingEvents(h){return JSON.parse(h.storage.get('portable-podcast-outbox-v2')||'[]');}
function endAudio(h){h.a.currentTime=600;h.a.paused=true;h.a.emit('ended');}

test('网页：结束后自动连播只关闭旧会话一次，下一集继续播放且没有永久冲突', async()=>{
  const h=harness();await h.flush();await h.ids.get('toggle-play').onclick();await h.flush();
  h.advance(1000,1);h.advance(1000,1);endAudio(h);await h.flush();
  const old=sessionEvents(h,'session-1'),terminal=old.filter(e=>['ended','stopped'].includes(e.state));
  assert.equal(h.a.src,'/audio/show/e2.mp3');assert.equal(h.a.paused,false);
  assert.equal(terminal.length,1);assert.equal(terminal[0].state,'ended');assert.equal(terminal[0].listened_ms,2000);
  assert.equal(old.at(-1).state,'ended');assert.equal(h.serverListened.get('session-1'),2000);
  assert.equal(pendingEvents(h).length,0);assert.equal(h.ids.get('sync-banner').hidden,true);
});

test('网页：结束后重新播放建立新会话，不再向已关闭会话提交停止记录', async()=>{
  const h=harness();await h.flush();h.ids.get('autoplay').checked=false;
  await h.ids.get('toggle-play').onclick();await h.flush();endAudio(h);await h.flush();
  const oldBefore=sessionEvents(h,'session-1');await h.ids.get('toggle-play').onclick();await h.flush();
  assert.deepEqual(sessionEvents(h,'session-1'),oldBefore);
  assert.equal(h.requests.filter(r=>r.url==='/api/listening/sessions').at(-1).body.restart,true);
  assert.equal(h.a.src,'/audio/show/e1.mp3');assert.equal(h.a.currentTime,0);assert.equal(h.a.paused,false);
  assert.equal(sessionEvents(h,'session-2')[0].state,'playing');assert.equal(pendingEvents(h).length,0);
});

test('网页：结束后离页、隐藏和迟到时间事件不再生成旧会话记录', async()=>{
  const h=harness();await h.flush();h.ids.get('autoplay').checked=false;
  await h.ids.get('toggle-play').onclick();await h.flush();endAudio(h);await h.flush();
  const before=sessionEvents(h,'session-1');h.fire('pagehide');h.fire('pagehide');
  h.document.hidden=true;for(const fn of h.docEvents.visibilitychange||[])fn({});
  h.a.emit('timeupdate');h.ids.get('speed').value='2';h.ids.get('speed').onchange();await h.flush();
  assert.deepEqual(sessionEvents(h,'session-1'),before);assert.equal(before.at(-1).state,'ended');
  assert.equal(h.beacons.length,0);assert.equal(pendingEvents(h).length,0);
});

test('网页：结束记录已提交但确认丢失，保留原内容重试且不阻挡下一集或重复计时', async()=>{
  const h=harness({terminalAckFailure:true});await h.flush();await h.ids.get('toggle-play').onclick();await h.flush();
  h.advance(1000,1);h.advance(1000,1);endAudio(h);await h.flush();
  const original=sessionEvents(h,'session-1').find(e=>e.state==='ended'),queued=pendingEvents(h).filter(e=>e.session_id==='session-1');
  assert.equal(h.a.src,'/audio/show/e2.mp3');assert.equal(h.a.paused,false);
  assert.equal(queued.length,1);assert.deepEqual(queued[0],original);assert.equal(original.listened_ms,2000);
  assert.equal(h.serverListened.get('session-1'),2000);assert.equal(h.ids.get('sync-banner').hidden,false);
  h.controls.terminalAckFailure=false;await h.ids.get('retry-sync').onclick();await h.flush();
  const retried=sessionEvents(h,'session-1').filter(e=>e.state==='ended');assert.ok(retried.length>=2);
  for(const event of retried)assert.deepEqual(event,original);
  assert.equal(sessionEvents(h,'session-1').some(e=>e.state==='stopped'),false);
  assert.equal([...h.acceptedEvents.values()].filter(e=>e.body.session_id==='session-1'&&e.body.state==='ended').length,1);
  assert.equal(h.serverListened.get('session-1'),2000);assert.equal(pendingEvents(h).length,0);assert.equal(h.ids.get('sync-banner').hidden,true);
});

test('网页：结束记录尚未到达后台时保留真实听时，恢复连接后原序号补交', async()=>{
  const h=harness({terminalFailure:true});await h.flush();await h.ids.get('toggle-play').onclick();await h.flush();
  h.advance(1000,1);h.advance(1000,1);endAudio(h);await h.flush();
  const original=sessionEvents(h,'session-1').find(e=>e.state==='ended');
  assert.equal(h.acceptedEvents.has(`session-1:${original.seq}`),false);
  assert.deepEqual(pendingEvents(h).find(e=>e.session_id==='session-1'),original);assert.equal(original.listened_ms,2000);
  assert.equal(h.a.src,'/audio/show/e2.mp3');assert.equal(h.a.paused,false);
  h.controls.terminalFailure=false;await h.ids.get('retry-sync').onclick();await h.flush();
  assert.deepEqual(h.acceptedEvents.get(`session-1:${original.seq}`).body,original);
  assert.equal(h.serverListened.get('session-1'),2000);assert.equal(pendingEvents(h).length,0);
});

test('网页：浏览排序变化后暂停续播仍沿用原收听顺序', async()=>{
  const h=harness();await h.flush();h.ids.get('episode-order').value='oldest';h.ids.get('episode-order').onchange();await h.flush();
  await h.ids.get('show-play').onclick();await h.flush();assert.equal(h.a.src,'/audio/show/e2.mp3');
  h.ids.get('episode-order').value='newest';h.ids.get('episode-order').onchange();await h.flush();
  await h.ids.get('toggle-play').onclick();await h.flush();assert.equal(h.ids.get('player').dataset.state,'paused');
  await h.ids.get('toggle-play').onclick();await h.flush();assert.equal(h.a.src,'/audio/show/e2.mp3');
  endAudio(h);await h.flush();assert.equal(h.a.src,'/audio/show/e1.mp3');assert.equal(h.a.paused,false);
  assert.equal(pendingEvents(h).length,0);
});

function catalogueTimer(h){return h.timers.find(t=>!t.interval&&[3000,6000,12000,15000].includes(t.delay));}
async function checkCatalogue(h){const timer=catalogueTimer(h);assert.ok(timer,'应存在有界目录检查');h.timers.splice(h.timers.indexOf(timer),1);h.advance(timer.delay,0);await timer.fn();await h.flush();}

test('首次目录：后台抓取完成后自动出现，只读目录且不自动播放', async()=>{
  const h=harness({catalogueEmpty:true});await h.flush();
  assert.equal(h.ids.get('catalogue-banner').hidden,false);assert.match(h.ids.get('catalogue-message').textContent,/正在抓取/);assert.match(h.ids.get('episode-list').innerHTML,/自动显示/);
  h.controls.catalogueEmpty=false;await checkCatalogue(h);
  assert.equal(h.ids.get('catalogue-banner').hidden,true);assert.match(h.ids.get('collection-count').innerHTML,/2 集/);assert.match(h.ids.get('episode-list').innerHTML,/中文单集一/);
  assert.equal(catalogueTimer(h),undefined);assert.equal(h.a.src,'');assert.equal(h.a.paused,true);
  assert.equal(h.requests.some(r=>r.url==='/api/refresh'||r.url==='/api/prepare'||r.url==='/api/listening/sessions'),false);
  assert.ok(h.requests.filter(r=>r.url==='/api/shows').every(r=>r.method==='GET'));
});

test('首次目录：联网失败最多40次读取，最终给重试；恢复只检查不重新转码', async()=>{
  const h=harness({catalogueEmpty:true});await h.flush();h.controls.catalogueOffline=true;
  for(let i=0;i<42&&catalogueTimer(h);i++)await checkCatalogue(h);
  assert.equal(h.requests.filter(r=>r.url==='/api/shows').length,41);assert.equal(catalogueTimer(h),undefined);
  assert.equal(h.ids.get('retry-catalogue').hidden,false);assert.match(h.ids.get('catalogue-message').textContent,/确认后台能联网/);
  h.controls.catalogueOffline=false;h.controls.catalogueEmpty=false;h.ids.get('retry-catalogue').onclick();await checkCatalogue(h);
  assert.equal(h.ids.get('catalogue-banner').hidden,true);assert.match(h.ids.get('episode-list').innerHTML,/中文单集一/);
  assert.equal(h.requests.some(r=>r.url==='/api/refresh'||r.url==='/api/prepare'),false);
});

test('首次目录：检查响应期间改选节目不被抢回，已有音频地址和播放保持不变', async()=>{
  const h=harness({catalogueEmpty:true,twoShows:true});await h.flush();await h.ids.get('toggle-play').onclick();await h.flush();
  const source=h.a.src,playerState=h.ids.get('player').dataset.state,sessions=h.requests.filter(r=>r.url==='/api/listening/sessions').length;
  h.controls.holdCatalogue=true;const timer=catalogueTimer(h);h.timers.splice(h.timers.indexOf(timer),1);const pending=timer.fn();await h.flush();
  h.ids.get('show-list').onclick({target:h.target({show:'second'},'[data-show]')});await h.flush();
  assert.equal(h.ids.get('show-heading').textContent,'另一个节目');
  h.controls.catalogueEmpty=false;h.controls.releaseCatalogue();await pending;await h.flush();
  assert.equal(h.ids.get('show-heading').textContent,'另一个节目');assert.equal(h.a.src,source);assert.equal(h.a.paused,false);assert.equal(h.ids.get('player').dataset.state,playerState);
  assert.equal(h.requests.filter(r=>r.url==='/api/listening/sessions').length,sessions);assert.equal(h.requests.some(r=>r.url==='/api/prepare'||r.url==='/api/refresh'),false);
});

test('首次目录：离开页面取消检查，后台标签超出10分钟也停止', async()=>{
  const leaving=harness({catalogueEmpty:true});await leaving.flush();leaving.fire('pagehide');assert.equal(catalogueTimer(leaving),undefined);
  const hidden=harness({catalogueEmpty:true});await hidden.flush();hidden.document.hidden=true;hidden.advance(600001,0);await checkCatalogue(hidden);
  assert.equal(hidden.requests.filter(r=>r.url==='/api/shows').length,1);assert.equal(catalogueTimer(hidden),undefined);assert.equal(hidden.ids.get('retry-catalogue').hidden,false);
});

test('首次目录：后台检查遇登录过期停止检查，不暂停或换掉正在播放的音频', async()=>{
  const h=harness({catalogueEmpty:true});await h.flush();await h.ids.get('toggle-play').onclick();await h.flush();const source=h.a.src;
  h.controls.catalogueStatus=401;await checkCatalogue(h);
  assert.equal(catalogueTimer(h),undefined);assert.match(h.ids.get('catalogue-message').textContent,/网页登录已失效/);assert.equal(h.a.src,source);assert.equal(h.a.paused,false);
});

test('首次目录：离页后的迟到响应不再更新页面或安排检查', async()=>{
  const h=harness({catalogueEmpty:true});await h.flush();h.controls.holdCatalogue=true;
  const timer=catalogueTimer(h);h.timers.splice(h.timers.indexOf(timer),1);const pending=timer.fn();await h.flush();
  h.fire('pagehide');h.controls.catalogueEmpty=false;h.controls.releaseCatalogue();await pending;await h.flush();
  assert.match(h.ids.get('collection-count').innerHTML,/0 集/);assert.equal(catalogueTimer(h),undefined);
});

test('首次目录：节目到达后的单集只读也不会因过期网页登录暂停当前音频', async()=>{
  const h=harness({catalogueEmpty:true});await h.flush();await h.ids.get('toggle-play').onclick();await h.flush();const source=h.a.src;
  h.controls.catalogueEmpty=false;h.controls.episodesStatus=401;await checkCatalogue(h);
  assert.equal(h.a.src,source);assert.equal(h.a.paused,false);assert.equal(catalogueTimer(h),undefined);
});

test('新增节目封面准备中会自动补图，不重载节目库或打断播放，失败重试有上限', async()=>{
  const h=harness();await h.flush();await h.ids.get('toggle-play').onclick();await h.flush();
  const before=h.requests.length,source=h.a.src;
  const image={tagName:'IMG',src:'/art/show',dataset:{},style:{visibility:'hidden'},isConnected:true,getAttribute(name){return name==='src'?this.src:null;}};
  const timers=h.timers.length;
  for(let retry=1;retry<=4;retry++){
    for(const fn of h.docEvents.error||[])fn({target:image});
    assert.equal(h.timers.length,timers+1);const timer=h.timers.pop();timer.fn();
    assert.equal(image.src,`/art/show?retry=${retry}`);
  }
  for(const fn of h.docEvents.error||[])fn({target:image});
  assert.equal(h.timers.length,timers);
  for(const fn of h.docEvents.load||[])fn({target:image});
  assert.equal(image.style.visibility,'');assert.equal(h.requests.length,before);
  assert.equal(h.a.src,source);assert.equal(h.a.paused,false);
});

test('封面重试不会碰外部图片，离开视图后的图片也不再下载', async()=>{
  const h=harness();await h.flush();const timers=h.timers.length;
  const image={tagName:'IMG',src:'https://outside.example/image.jpg',dataset:{},style:{},isConnected:true,getAttribute(){return this.src;}};
  for(const fn of h.docEvents.error||[])fn({target:image});assert.equal(h.timers.length,timers);
  image.src='/art/show';for(const fn of h.docEvents.error||[])fn({target:image});
  const timer=h.timers.pop();image.isConnected=false;timer.fn();assert.equal(image.src,'/art/show');
});
