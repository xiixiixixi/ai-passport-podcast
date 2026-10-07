(() => {
  'use strict';
  const $ = id => document.getElementById(id), audio = $('audio');
  const { ListeningMeter, EventOutbox, clock, listening, status } = PodcastCore;
  const PAGE_SIZE = 10;
  const KEYS = {client: 'portable-podcast-client-v2', outbox: 'portable-podcast-outbox-v2', sessions: 'portable-podcast-sessions-v2', volume: 'portable-podcast-volume-v1', migration: 'portable-podcast-migration-v2'};
  let shows = [], selectedShow = null, episodes = [], offset = 0, total = 0, pageRequest = 0;
  let libraryState = 'loading', episodeFilter = 'all', episodeOrder = 'newest', recent = [], statistics = null;
  let current = null, state = 'idle', requestVersion = 0, pollTimer = null, pendingSeek = null, wantsPlay = false;
  let sleepDeadline = 0, lastReport = 0, toastTimer = null, syncFailure = '', syncRefreshing = false;
  let sourceVersion = 0, sourcePreview = null, sourceAdding = false, searchVersion = 0, searchResults = [], recentExpanded = false;
  const catalogueWait = {active:false, startedOnce:false, started:0, checks:0, timer:null, generation:0};
  const meter = new ListeningMeter(), progressByEpisode = new Map();
  const legacyMarks=read('portable-podcast-progress-v1',{}),legacyLast=read('portable-podcast-last-v1',null),migrationState=read(KEYS.migration,[]);
  const legacyDone=new Set(Array.isArray(migrationState)?migrationState:[]),legacyRecords=new Map();
  for(const record of [...(legacyMarks&&typeof legacyMarks==='object'?Object.values(legacyMarks):[]),legacyLast]){
    if(record&&typeof record.sid==='string'&&record.sid.length<=128&&typeof record.eid==='string'&&record.eid.length<=128&&(Number(record.position)>0||record.finished===true))legacyRecords.set(`${record.sid}/${record.eid}`,record);
  }
  let migrationDismissed=false,importingLegacy=false;
  const statusText = {idle: '等你按下播放', restored: '接着上次听', loading: '正在读取这一集', preparing: '正在准备音频', buffering: '正在缓冲', playing: '正在播放', paused: '已暂停', finished: '本集播放结束', error: '播放遇到问题'};
  const icons = name => `<span class="icon icon-${name}" aria-hidden="true"></span>`;
  const escape = value => String(value ?? '').replace(/[&<>"']/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
  const key = (sid, eid) => `${sid}/${eid}`;
  const uuid = () => typeof crypto.randomUUID === 'function' ? crypto.randomUUID() : `${Date.now().toString(36)}-${Math.random().toString(36).slice(2)}-${Math.random().toString(36).slice(2)}`;
  function read(name, fallback) { try { return JSON.parse(localStorage.getItem(name)) ?? fallback; } catch { return fallback; } }
  function write(name, value) { try { localStorage.setItem(name, JSON.stringify(value)); return true; } catch { return false; } }
  const storedClient = read(KEYS.client, null);
  const clientId = typeof storedClient === 'string' && storedClient.startsWith('web-') ? storedClient : `web-${uuid()}`;
  write(KEYS.client, clientId);
  const storedSessions=read(KEYS.sessions, {});
  const sessionEpisodes=storedSessions&&typeof storedSessions==='object'&&!Array.isArray(storedSessions)?storedSessions:{};
  function toast(text) { clearTimeout(toastTimer); $('toast').textContent = text; $('toast').hidden = false; toastTimer = setTimeout(() => $('toast').hidden = true, 5000); }
  function date(value) { const s = String(value || ''); return /^\d{4}-\d{2}-\d{2}/.test(s) ? s.slice(0,10).replaceAll('-', '.') : '日期待确认'; }
  function duration(value) { const n = Math.max(0, Math.round(Number(value) || 0)); return n ? `${Math.floor(n / 60)} 分钟` : '时长待确认'; }
  function artwork(value, sid) { if (typeof value === 'string' && value.startsWith('/') && !value.startsWith('//')) return value; try { const u = new URL(value); if (u.protocol === 'https:' || u.protocol === 'http:') return u.href; } catch {} return sid ? '/art/' + encodeURIComponent(sid) : '/static/assets/icons/headphones.svg'; }
  // A newly added source may still be preparing its artwork. Retry only our
  // local cover endpoint, for a bounded time, without reloading the library.
  function localCover(image) {
    if (!image || image.tagName !== 'IMG') return null;
    const source = image.getAttribute('src') || '';
    const match = source.match(/^\/art\/([A-Za-z0-9_-]{1,23})(?:\?[^#]*)?$/);
    return match ? '/art/' + match[1] : null;
  }
  document.addEventListener('error', event => {
    const image = event.target, path = localCover(image);
    if (!path) return;
    const attempts = Number(image.dataset.coverRetries) || 0;
    if (attempts >= 4) return;
    image.dataset.coverRetries = String(attempts + 1);
    setTimeout(() => {
      if (image.isConnected && localCover(image) === path) image.src = path + '?retry=' + (attempts + 1);
    }, 2000 * 2 ** attempts);
  }, true);
  document.addEventListener('load', event => {
    if (localCover(event.target)) event.target.style.visibility = '';
  }, true);
  async function api(path, options = {}) {
    const controller = new AbortController(), timer = setTimeout(() => controller.abort(), options.timeout || 20000);
    const {timeout, quietAuth, ...init} = options;
    try {
      const response = await fetch(path, {...init, signal: controller.signal});
      if (response.status === 401 && !quietAuth) { wantsPlay = false; audio.pause(); location.replace('/login?next=' + encodeURIComponent('/' + location.hash)); }
      let body; try { body = await response.json(); } catch { throw new Error('后台没有返回可读取的内容，请稍后重试。'); }
      if (!response.ok) { const message=String(body.error||''); const translations={'show not found':'这档节目已不在节目库里，请重新选择或添加。','episode not found':'没有找到这集节目。可以刷新节目库后重试。','invalid pagination':'没有找到这一页，请重新选择节目。'}; const error = new Error(translations[message]||(/[\u4e00-\u9fff]/.test(message)?message:'后台暂时无法完成操作，请稍后重试。')); error.status = response.status; throw error; }
      return body;
    } catch (error) { if (error.name === 'AbortError') throw new Error('连接后台超时，请稍后重试。'); throw error; }
    finally { clearTimeout(timer); }
  }
  const post = (path, body, options = {}) => api(path, {...options, method:'POST', headers:{'Content-Type':'application/json'}, body:JSON.stringify(body)});
  function updateProgress(sid, eid, progress) {
    if (!progress) return;
    const previous = progressByEpisode.get(key(sid, eid));
    if (!previous || Number(progress.revision || 0) >= Number(previous.revision || 0)) progressByEpisode.set(key(sid, eid), {...previous,...progress});
  }
  function progressFor(sid, episode) { return progressByEpisode.get(key(sid, episode.id || episode.episode_id)) || episode.progress || {status:'unplayed', position_ms:0, listened_ms:0, play_count:0}; }
  function syncBanner(message) { syncFailure = message || ''; $('sync-message').textContent = syncFailure; $('sync-banner').hidden = !syncFailure; }
  const outbox = new EventOutbox({
    events: read(KEYS.outbox, []),
    save: events => write(KEYS.outbox, events),
    send: event => post('/api/listening/events', event, {keepalive:true}),
    changed: ({pending, durable, error}) => {
      if (!durable) syncBanner('浏览器未允许保留待同步记录。请保持页面打开，恢复连接后重新同步。');
      else if (error) syncBanner('收听记录暂时没有同步到后台。播放可以继续，我们会自动重试；先别关闭页面。');
      else if (!pending && syncFailure.startsWith('收听记录暂时')) syncBanner('');
    },
    received: (response, event) => {
      const identity = sessionEpisodes[event.session_id];
      if (identity && response.progress) updateProgress(identity.sid, identity.eid, response.progress);
      if (current?.sessionId === event.session_id && response.stale) current.stale = true;
      renderEpisodes(); renderRecent();
    }
  });
  function fullDuration() { return current ? current.duration || 0 : 0; }
  function position() { if (!current) return 0; if (!current.audioUrl) return current.savedPosition || 0; const local = pendingSeek ?? (Number(audio.currentTime) || 0); return local; }
  function sampleListening() { if (!current?.sessionId) return; meter.sample(performance.now(), position(), state === 'playing' && wantsPlay && !audio.paused && !audio.muted && audio.volume > 0 && audio.readyState >= 3, audio.playbackRate, audio.seeking || pendingSeek !== null); }
  function report(force = false, eventState, seek = false) {
    if (!current?.sessionId || current.terminalQueued) return null;
    sampleListening();
    if (!force && Date.now() - lastReport < 10000) return null;
    lastReport = Date.now();
    const event = {session_id:current.sessionId, seq:current.nextSeq++, position_ms:Math.max(0,Math.round(position()*1000)), listened_ms:Math.max(0,Math.round(meter.total)), rate: Number(audio.playbackRate) || 1, state:eventState || (state==='finished'?'ended':state==='playing'?'playing':'paused')};
    if (seek) event.seek = true;
    outbox.add(event);
    // Queued terminal events still retry until acknowledged; this session cannot emit more.
    if (event.state === 'ended' || event.state === 'stopped') current.terminalQueued = true;
    void outbox.drain(); return event;
  }
  function updateTime() {
    const length = fullDuration(), pos = state === 'finished' ? length : position();
    $('elapsed').textContent = clock(pos); $('duration').textContent = length ? clock(length) : '0:00';
    $('seek').max = Math.max(1,length); if (document.activeElement !== $('seek')) $('seek').value = pos;
    $('seek').style.setProperty('--progress', `${length ? Math.min(100,pos/length*100) : 0}%`);
  }
  function updateOptionsSummary() { const timed = Number($('sleep').value); $('options-summary').textContent = `${$('speed').selectedOptions[0].textContent} · ${timed ? timed+'分钟定时' : '未定时'}`; }
  function updateDock() {
    const show = shows.find(s => s.id === current?.sid);
    const source = artwork(show?.art || current?.art, current?.sid);
    if ($('player-art').getAttribute('src') !== source) $('player-art').src = source;
    $('player-art').alt = current ? `${current.showName}节目封面` : '随身听备用图标';
    $('player-title').textContent = current?.title || '选择一集开始收听';
    $('player-title').title = current?.title || ''; $('player-show').textContent = current?.showName || '随身听 · 播客播放器';
    $('options-title').textContent = current?.title || '选择一集开始收听';
    $('player-detail').ariaLabel = current ? `收听选项：${current.title}` : '打开收听选项';
    updateOptionsSummary();
  }
  function setState(next, note) {
    state = next; $('player').dataset.state = next; $('player-status').textContent = statusText[next] || next;
    if (note !== undefined) $('player-note').textContent = note;
    const has = Boolean(current), loading = ['loading','preparing'].includes(next);
    $('toggle-play').disabled = !has;
    $('toggle-play').ariaLabel = loading ? (wantsPlay ? '取消准备后的自动播放' : '准备完成后播放') : ({playing:'暂停',buffering:'暂停',finished:'重新播放',error:'重试播放',restored:'继续收听',paused:'继续播放'}[next] || '播放');
    $('toggle-play').innerHTML = icons((['playing','buffering'].includes(next) || loading) && wantsPlay ? 'pause' : 'play');
    const seekable = has && Boolean(current.audioUrl) && !loading && next !== 'error';
    ['back-15','forward-15','seek','restart'].forEach(id => $(id).disabled = !seekable);
    $('newer-episode').disabled = !has || loading || !(current.order==='oldest'?current.older:current.newer); $('older-episode').disabled = !has || loading || !(current.order==='oldest'?current.newer:current.older);
    updateTime(); renderEpisodes(); updateDock();
  }
  function renderShows() {
    if (!shows.length) { $('show-list').innerHTML = '<p class="empty">还没有订阅节目。<a href="#sources">添加第一档节目</a>，然后在网页或设备上收听。</p>'; $('sidebar-shows').innerHTML = ''; const emptyManage = $('manage-shows'); if (emptyManage) emptyManage.innerHTML = '<p class="form-hint" style="border:0;padding:4px 0">暂无订阅节目。</p>'; return; }
    $('show-list').innerHTML = shows.map(s => {
      const stat = statistics?.shows?.find(row => row.show_id === s.id) || s.listening;
      const cached = Number(s.cached_episodes) || 0;
      return `<button class="channel" data-show="${escape(s.id)}" aria-pressed="${selectedShow?.id===s.id}"><span class="cover-wrap"><span class="cover-fallback">${icons('headphones')}</span><img src="${escape(artwork(s.art,s.id))}" alt="${escape(s.name)}节目封面" loading="lazy" onerror="this.style.visibility='hidden'">${selectedShow?.id===s.id?'<span class="cover-selected">正在浏览</span>':''}</span><span class="channel-name">${escape(s.name)}</span><span class="channel-count">${Number(s.episodes)||0} 集${cached?` · <span class="cache-badge is-cached">已缓存 ${cached} 集</span>`:''}${stat?.played_episodes?` · 听过 ${Number(stat.played_episodes)} 集`:''}</span></button>`;
    }).join('');
    $('sidebar-shows').innerHTML = shows.map(s => `<button class="sidebar-show" data-show="${escape(s.id)}" aria-pressed="${selectedShow?.id===s.id}"><img src="${escape(artwork(s.art,s.id))}" alt="" loading="lazy" onerror="this.style.visibility='hidden'"><span>${escape(s.name)}</span></button>`).join('');
    const manageList = $('manage-shows');
    if (manageList) manageList.innerHTML = shows.map(s => `<div style="display:flex;align-items:center;justify-content:space-between;gap:12px;padding:9px 0;border-top:1px solid rgba(125,135,150,.25)"><span style="overflow:hidden;text-overflow:ellipsis;white-space:nowrap" title="${escape(s.name)}">${escape(s.name)}</span><button class="quiet-button" data-unsub="${escape(s.id)}">退订</button></div>`).join('');
    renderShowStats(); updateDock();
  }
  function renderShowStats() {
    const stat = statistics?.shows?.find(row => row.show_id === selectedShow?.id) || selectedShow?.listening;
    $('show-listening-summary').innerHTML = stat ? `<span>听过 <strong>${Number(stat.played_episodes)||0}</strong> 集</span><span>听完 <strong>${Number(stat.completed_episodes)||0}</strong> 集</span><span>播放 <strong>${Number(stat.play_count)||0}</strong> 次</span><span>听了 <strong>${listening(stat.listened_ms)}</strong></span><a href="#stats">全部统计</a>` : '';
  }
  function renderRecent() {
    const history=[...recent];
    for(const record of [...legacyRecords.values()].sort((a,b)=>(Number(b.updated)||0)-(Number(a.updated)||0))){const k=key(record.sid,record.eid),remote=progressByEpisode.get(k);if(!legacyDone.has(k)&&(!remote||(!Number(remote.revision)&&remote.status==='unplayed'))&&!history.some(e=>e.show_id===record.sid&&e.episode_id===record.eid))history.push({show_id:record.sid,episode_id:record.eid,show_name:record.showName||'历史收听',title:record.title||'历史单集',duration:Number(record.duration)||0,legacy:true,progress:{status:record.finished?'completed':'in_progress',position_ms:Math.max(0,Math.round(Number(record.position)*1000))}});}
    updateMigrationBanner();
    if (!history.length) { $('recent-more').hidden=true; $('recent-list').innerHTML = '<p class="recent-empty">听过的单集会出现在这里。网页和设备都可以接着听。</p>'; return; }
    $('recent-more').hidden=history.length<=3; $('recent-more').textContent=recentExpanded?'收起':'查看更多';
    $('recent-list').innerHTML = history.slice(0,recentExpanded?20:3).map(e => {
      const progress = e.legacy?e.progress:progressFor(e.show_id,{id:e.episode_id,progress:e.progress}), completed = progress.status === 'completed';
      const percent = e.duration ? Math.min(100,(Number(progress.position_ms)||0)/10/e.duration) : 0;
      return `<button class="recent-item" data-recent-show="${escape(e.show_id)}" data-recent-episode="${escape(e.episode_id)}"><img src="${escape(artwork(e.art,e.show_id))}" alt="" onerror="this.style.visibility='hidden'"><span class="recent-copy"><span class="recent-show">${escape(e.show_name)}${e.legacy||progress.legacy?' · 历史进度':''}</span><strong>${escape(e.title)}</strong><span class="recent-position">${completed?'已听完 · 再听一次':`听到 ${clock((Number(progress.position_ms)||0)/1000)} · 继续收听`}</span><span class="recent-progress" aria-hidden="true"><span style="width:${percent}%"></span></span></span><span class="recent-play" aria-hidden="true">${icons('play')}</span></button>`;
    }).join('');
  }
  function renderEpisodes() {
    if (catalogueWait.active && selectedShow && !Number(selectedShow.episodes) && !episodes.length) { $('show-play').disabled = true; $('episode-list').innerHTML = '<p class="empty" role="status">后台正在抓取这档节目的单集，读取完成后会自动显示。</p>'; return; }
    if (libraryState === 'loading') { $('episode-list').innerHTML = '<div role="status" aria-label="正在读取单集"><div class="skeleton-row"></div><div class="skeleton-row"></div><div class="skeleton-row"></div></div>'; $('show-play').disabled = true; return; }
    if (libraryState === 'error') { $('show-play').disabled = true; $('episode-list').innerHTML = '<div class="empty">暂时没能读到单集。<br><button class="quiet-button" data-retry-list>重新读取</button></div>'; return; }
    $('show-play').disabled = !episodes.length;
    $('show-play').innerHTML = `${icons('play')}${episodeFilter === 'in_progress' ? '接着听本页第一集' : '播放本页第一集'}`;
    if (!episodes.length) { $('episode-list').innerHTML = `<div class="empty">${episodeFilter==='all'?'这档节目还没有单集。可以刷新订阅源。':'这个分类暂时没有单集。切换到“全部”查看其他内容。'}</div>`; return; }
    $('episode-list').innerHTML = episodes.map((e,i) => {
      const active = current?.sid === selectedShow?.id && current?.eid === e.id, p = progressFor(selectedShow.id,e);
      const resume = p.status==='in_progress', badge = status(p);
      const cached = e.ready === true;
      const label = active && ['playing','buffering'].includes(state) ? '暂停' : resume ? '继续收听' : p.status==='completed' ? '重新播放' : '播放';
      return `<article class="episode ${active?'active':''}"><span class="episode-number">${active?icons('headphones'):offset+i+1}</span><div class="episode-body"><h3>${escape(e.title)}</h3><p class="episode-meta"><span>${date(e.pub_date||e.published)}</span><span>${duration(e.duration)}</span><span class="cache-badge ${cached?'is-cached':'is-missing'}">${cached?'已缓存':'未缓存'}</span><span class="listen-status status-${escape(p.status||'unplayed')}">${badge}${resume?` · ${clock((Number(p.position_ms)||0)/1000)}`:''}</span>${Number(p.play_count)>0?`<span>播放 ${Number(p.play_count)} 次</span>`:''}${active?`<span class="episode-current">${statusText[state]}</span>`:''}</p></div><div class="episode-actions"><button class="small-play" data-episode="${escape(e.id)}" aria-label="${escape(label)}：${escape(e.title)}">${icons(active && wantsPlay && ['playing','buffering','loading','preparing'].includes(state)?'pause':'play')}</button><details class="episode-menu"><summary aria-label="单集标记：${escape(e.title)}">···</summary><div><button data-mark="completed" data-mark-episode="${escape(e.id)}">标记听完</button><button data-mark="unplayed" data-mark-episode="${escape(e.id)}">标记未听</button></div></details></div></article>`;
    }).join('');
  }
  function updatePagination(){
    $('page-info').textContent=total?`第 ${Math.floor(offset/PAGE_SIZE)+1} / ${Math.ceil(total/PAGE_SIZE)} 页 · ${offset+1} 至 ${Math.min(offset+PAGE_SIZE,total)} 集${episodeFilter!=='all'?' · 当前分类':''}`:'暂无单集';
    $('previous-page').disabled=offset===0;$('next-page').disabled=offset+PAGE_SIZE>=total;
  }
  async function loadEpisodes(sid,nextOffset=0,{quietAuth=false}={}) {
    const version = ++pageRequest, show = shows.find(s => s.id === sid); if (!show) return;
    selectedShow=show; episodes=[]; libraryState='loading'; renderEpisodes(); renderShows();
    $('show-heading').textContent=show.name; $('show-author').textContent=show.author||'播客节目';
    $('show-cover').style.visibility='visible'; $('show-cover').src=artwork(show.art,show.id); $('show-cover').alt=show.name+'节目封面';
    $('show-summary').textContent='正在读取单集…'; $('episode-list').ariaBusy='true'; $('previous-page').disabled=$('next-page').disabled=true;
    const filter = episodeFilter==='all'?'':`&status=${encodeURIComponent(episodeFilter)}`;
    try {
      const page = await api(`/api/shows/${encodeURIComponent(sid)}/episodes?web=1&offset=${nextOffset}&limit=${PAGE_SIZE}&order=${episodeOrder}${filter}`,{quietAuth});
      if (version!==pageRequest) return;
      if (page.total>0 && page.offset>=page.total) { await loadEpisodes(sid,Math.floor((page.total-1)/PAGE_SIZE)*PAGE_SIZE,{quietAuth}); return; }
      episodes=page.episodes||[]; libraryState='ready'; offset=page.offset||0; total=page.total||0;
      episodes.forEach(e=>updateProgress(sid,e.id,e.progress));
      $('show-summary').textContent=`${Number(show.episodes)||0} 集 · 浏览不打断收听`;
      updatePagination(); renderEpisodes(); renderShowStats();
    } catch { if (version!==pageRequest) return; libraryState='error'; $('show-summary').textContent='单集读取失败'; renderEpisodes(); }
    finally { if (version===pageRequest) $('episode-list').ariaBusy='false'; }
  }
  function updateCatalogue(body) {
    const previous=JSON.stringify(shows);
    shows=body.shows||[];
    if(selectedShow) selectedShow=shows.find(show=>show.id===selectedShow.id)||selectedShow;
    const count=shows.reduce((n,show)=>n+(Number(show.episodes)||0),0);
    $('collection-count').innerHTML=`<strong>${shows.length} 档节目</strong> · ${count.toLocaleString('zh-CN')} 集`;
    if(previous!==JSON.stringify(shows)) renderShows();
  }
  function finishCatalogueWait({failed=false}={}) {
    clearTimeout(catalogueWait.timer); catalogueWait.timer=null; catalogueWait.active=false; catalogueWait.generation++;
    $('catalogue-banner').hidden=!failed; $('retry-catalogue').hidden=!failed;
    $('catalogue-login').hidden=true;
    if(failed) $('catalogue-message').textContent='暂时还没有读到当前节目的单集。请确认后台能联网，再重新检查；也可以用上方“刷新订阅源”重试抓取。已有单集可以继续收听。';
    renderEpisodes();
  }
  function scheduleCatalogueCheck() {
    if(!catalogueWait.active || catalogueWait.timer) return;
    const delay=Math.min(15000,3000*2**Math.min(catalogueWait.checks,3));
    catalogueWait.timer=setTimeout(checkInitialCatalogue,delay);
  }
  function startCatalogueWait() {
    finishCatalogueWait();
    catalogueWait.active=true; catalogueWait.startedOnce=true; catalogueWait.started=Date.now(); catalogueWait.checks=0;
    $('catalogue-banner').hidden=false; $('retry-catalogue').hidden=true;
    $('catalogue-message').textContent='后台正在抓取节目，单集会自动出现。你可以先连接设备，或继续浏览。';
    renderEpisodes(); scheduleCatalogueCheck();
  }
  async function checkInitialCatalogue() {
    catalogueWait.timer=null;
    if(!catalogueWait.active) return;
    if(catalogueWait.checks>=40 || Date.now()-catalogueWait.started>=600000) { finishCatalogueWait({failed:true}); return; }
    if(document.hidden) { scheduleCatalogueCheck(); return; }
    const generation=catalogueWait.generation; catalogueWait.checks++;
    try {
      // Read existing catalogue only. Never start a refresh, preparation or playback.
      const body=await api('/api/shows',{timeout:8000,quietAuth:true});
      if(!catalogueWait.active || generation!==catalogueWait.generation) return;
      updateCatalogue(body);
      const selected=selectedShow && shows.find(show=>show.id===selectedShow.id);
      if(selected && Number(selected.episodes)>0) {
        finishCatalogueWait();
        // Read the current selection at response time; no captured old selection wins.
        await loadEpisodes(selected.id,offset,{quietAuth:true});
        return;
      }
      if(!selectedShow && shows.some(show=>Number(show.episodes)>0)) { finishCatalogueWait(); return; }
      $('catalogue-message').textContent=shows.some(show=>Number(show.episodes)>0)?'节目正在陆续更新，当前节目读取完成后会自动显示。已有单集可以先收听。':'后台正在抓取节目，单集会自动出现。你可以先连接设备，或继续浏览。';
    } catch(error) {
      if(!catalogueWait.active || generation!==catalogueWait.generation) return;
      if(error.status===401 || error.status===403) { finishCatalogueWait({failed:true}); $('catalogue-message').textContent='网页登录已失效，请重新登录后台后检查。'; $('retry-catalogue').hidden=true; $('catalogue-login').hidden=false; return; }
      $('catalogue-message').textContent='暂时未连上后台，稍后会自动检查。请确认后台仍在运行。';
    }
    scheduleCatalogueCheck();
  }
  $('retry-catalogue').onclick=()=>startCatalogueWait();
  window.addEventListener('pagehide',()=>finishCatalogueWait());
  const gb = n => `${(Math.max(0, Number(n) || 0) / 1073741824).toFixed(1)} GB`;
  function ensureOption(select, value) {
    if (![...select.options].some(option => option.value === String(value))) {
      const option = document.createElement('option');
      option.value = String(value); option.textContent = `自定义 ${value}`;
      select.appendChild(option);
    }
    select.value = String(value);
  }
  async function loadCachePanel() {
    const note = $('storage-note'); if (!note) return;
    try {
      const [settings, storage] = await Promise.all([api('/api/settings'), api('/api/storage')]);
      note.textContent = `单集缓存 ${Number(storage.ready_episodes) || 0} 集已就绪，占用 ${gb(storage.media_bytes)}；磁盘剩余 ${gb(storage.disk_free_bytes)}。每天凌晨 3 点左右自动清理，每档至少保留最近 ${Number(settings.keep_per_show) || 3} 集。`;
      ensureOption($('prefetch-latest'), Number(settings.prefetch_latest) || 3);
      ensureOption($('keep-per-show'), Number(settings.keep_per_show) || 3);
      const last = settings.last_cleanup || {};
      $('cleanup-last').textContent = Number(last.finished_at) > 0
        ? `上次清理 ${new Date(Number(last.finished_at) * 1000).toLocaleString('zh-CN', {hour12: false})}：删除 ${Number(last.deleted) || 0} 集，释放 ${gb((Number(last.freed_mb) || 0) * 1048576)}。`
        : '还没有执行过清理。首次部署后可以点"立即清理"检查一次。';
    } catch { note.textContent = '缓存用量暂时读取不到，不影响收听。'; }
  }
  async function waitMaintenance() {
    const until = Date.now() + 180000;
    while (Date.now() < until) {
      const state = await api('/api/maintenance', {timeout: 10000});
      if (!state.running) return state;
      await new Promise(resolve => setTimeout(resolve, 1500));
    }
    return null;
  }
  async function putSetting(name, value, message) {
    const feedback = $('cache-feedback');
    try {
      await api('/api/settings', {method: 'PUT', headers: {'Content-Type': 'application/json'}, body: JSON.stringify({[name]: value})});
      feedback.textContent = '';
      toast(message);
      void loadCachePanel();
    } catch (error) { feedback.textContent = error.message || '设置没有保存，请稍后重试。'; }
  }
  async function loadShows() {
    try {
      const body=await api('/api/shows'); updateCatalogue(body);
      const empty=shows.length>0 && shows.every(show=>!Number(show.episodes));
      if(empty && !catalogueWait.startedOnce) startCatalogueWait();
      else if(!empty && catalogueWait.active && selectedShow && Number(selectedShow.episodes)>0) finishCatalogueWait();
      const sid=selectedShow?.id||current?.sid||shows[0]?.id;
      if(sid) await loadEpisodes(shows.some(s=>s.id===sid)?sid:shows[0].id,selectedShow?.id===sid?offset:0);
      else { libraryState='ready'; episodes=[]; renderEpisodes(); $('episode-list').ariaBusy='false'; }
    } catch { libraryState='error'; renderEpisodes(); $('episode-list').ariaBusy='false'; $('collection-count').textContent='节目暂时无法连接'; $('show-list').innerHTML='<p class="empty">节目读取失败。<br><button class="quiet-button" id="retry-shows">重新读取</button></p>'; $('retry-shows').onclick=loadShows; }
  }
  function renderStatistics() {
    if(!statistics)return;
    const s=statistics.summary||{};
    $('stats-summary').innerHTML=`<div><span>总听时长</span><strong>${listening(s.listened_ms)}</strong></div><div><span>听过的单集</span><strong>${Number(s.played_episodes)||0}<small> 集</small></strong></div><div><span>听完的单集</span><strong>${Number(s.completed_episodes)||0}<small> 集</small></strong></div><div><span>实际播放</span><strong>${Number(s.play_count)||0}<small> 次</small></strong></div>`;
    $('stats-summary').ariaBusy='false';
    const rows=[...(statistics.shows||[])].sort((a,b)=>(Number(b.listened_ms)||0)-(Number(a.listened_ms)||0));
    $('show-stats').innerHTML=rows.length?rows.map(row=>`<button class="show-stat-row" data-show="${escape(row.show_id)}"><span class="stat-show-name">${escape(row.name)}</span><span>听过 ${Number(row.played_episodes)||0} 集</span><span>听完 ${Number(row.completed_episodes)||0} 集</span><span>${Number(row.play_count)||0} 次</span><strong>${listening(row.listened_ms)}</strong>${icons('right')}</button>`).join(''):'<p class="empty">添加节目并开始收听后，会在这里显示。</p>';
    const days=[]; for(let i=6;i>=0;i--){const d=new Date(Date.now()-i*86400000),parts=new Intl.DateTimeFormat('en-CA',{timeZone:statistics.timezone||'Asia/Shanghai',year:'numeric',month:'2-digit',day:'2-digit'}).formatToParts(d),fields=Object.fromEntries(parts.map(p=>[p.type,p.value]));const label=`${fields.year}-${fields.month}-${fields.day}`;const found=(statistics.days||[]).find(row=>row.date===label);days.push({label,day:Number(fields.day),month:Number(fields.month),listened:Number(found?.listened_ms)||0});}
    const peak=Math.max(60000,...days.map(d=>d.listened));
    $('listening-days').innerHTML=days.map(d=>`<div class="listening-day"><span class="day-duration">${listening(d.listened)}</span><div class="day-chart" aria-hidden="true"><span style="height:${Math.max(2,d.listened/peak*100)}%"></span></div><span>${d.month}.${d.day}</span></div>`).join('');
    renderShowStats(); renderShows();
  }
  async function refreshListening({quiet=false}={}) {
    if(syncRefreshing)return;syncRefreshing=true;
    try {
      const results=await Promise.allSettled([api('/api/listening/recent?limit=20'),api('/api/listening/stats')]);
      if(results[0].status==='fulfilled') {
        recent=results[0].value.episodes||[];recent.forEach(e=>updateProgress(e.show_id,e.episode_id,e.progress));renderRecent();$('recent-list').ariaBusy='false';
        if(!current && recent[0] && recent[0].progress?.status!=='completed') {
          const e=recent[0];current={sid:e.show_id,eid:e.episode_id,showName:e.show_name,title:e.title,art:e.art,duration:e.duration||0,audioUrl:null,savedPosition:(Number(e.progress?.position_ms)||0)/1000};setState('restored','进度来自后台。点播放接着听，不会自动播放。');
        }
      } else { $('recent-note').textContent='收听记录暂时未连接'; if(!recent.length)$('recent-list').innerHTML='<p class="recent-empty">暂时无法读取记录。<button class="quiet-button" data-retry-sync>重新读取</button></p>'; }
      if(results[1].status==='fulfilled'){statistics=results[1].value;renderStatistics();}
      else if(!statistics){$('stats-summary').innerHTML='<p class="empty">统计暂时无法连接。请点“更新统计”重试。</p>';}
      if(results.some(r=>r.status==='rejected')){if(!quiet)toast('部分收听记录未更新，稍后可以重试。');}
      else { $('recent-note').textContent='网页与设备共用'; if(syncFailure.startsWith('收听记录暂时')&&!outbox.events.length)syncBanner(''); }
      if(selectedShow && libraryState==='ready') {
        const filter=episodeFilter==='all'?'':`&status=${encodeURIComponent(episodeFilter)}`;
        const sid=selectedShow.id,version=pageRequest;
        const page=await api(`/api/shows/${encodeURIComponent(sid)}/episodes?web=1&offset=${offset}&limit=${PAGE_SIZE}&order=${episodeOrder}${filter}`);
        if(version===pageRequest){total=page.total||0;if(offset>0&&offset>=total){await loadEpisodes(sid,total?Math.floor((total-1)/PAGE_SIZE)*PAGE_SIZE:0);}else{episodes=page.episodes||[];episodes.forEach(e=>updateProgress(sid,e.id,e.progress));updatePagination();renderEpisodes();}}
      }
    } catch { if(!quiet)toast('收听记录未更新，请稍后重试。'); }
    finally {syncRefreshing=false;}
  }
  function clearPolling(){clearTimeout(pollTimer);pollTimer=null;}
  async function startEpisode(show,episode,{restart=false,order=episodeOrder}={}) {
    report(true,'stopped'); clearPolling(); const version=++requestVersion;
    wantsPlay=false; sampleListening(); audio.pause(); audio.removeAttribute('src'); audio.load(); pendingSeek=null; meter.reset();
    current={sid:show.id,eid:episode.id,showName:show.name,title:episode.title,art:show.art,duration:episode.duration||0,audioUrl:null,savedPosition:0,sessionId:null,nextSeq:1,terminalQueued:false,newer:null,older:null,order};
    wantsPlay=true; setState('loading','正在读取音频与共享进度。');
    const identity={sid:current.sid,eid:current.eid};
    try {
      const detail=await api(`/api/episodes/${encodeURIComponent(identity.sid)}/${encodeURIComponent(identity.eid)}`);if(version!==requestVersion)return;
      const old=legacyRecords.get(key(identity.sid,identity.eid));if(old&&!legacyDone.has(key(identity.sid,identity.eid))&&!Number(detail.progress?.revision)&&(!detail.progress?.status||detail.progress.status==='unplayed')){await importLegacyRecords([old],{quiet:true});if(version!==requestVersion)return;}
      const sessionBody={client_id:clientId,show_id:identity.sid,episode_id:identity.eid,request_id:uuid()};
      if(restart)sessionBody.restart=true;
      await outbox.drain();if(version!==requestVersion)return;
      const session=await post('/api/listening/sessions',sessionBody);if(version!==requestVersion)return;
      current.sessionId=session.session_id;current.nextSeq=Number(session.next_seq)||1;current.savedPosition=Math.max(0,Number(session.position_ms)||0)/1000;
      current.newer=detail.newer;current.older=detail.older;sessionEpisodes[session.session_id]=identity;write(KEYS.sessions,sessionEpisodes);updateProgress(identity.sid,identity.eid,session.progress);
      if(!detail.browser_audio_ready){let prepared=null;try{prepared=await post('/api/prepare',{show_id:identity.sid,episode_id:identity.eid});}catch{prepared=null;}if(version!==requestVersion)return;const eta=Math.ceil((Number(prepared?.eta_seconds)||0)/60),estimate=eta>0?`，预计约 ${eta} 分钟`:'';setState('preparing',wantsPlay?`正在缓存这一集${estimate}。你可以继续浏览，也可以点暂停，准备好后不会自动播放。`:`正在缓存这一集${estimate}，完成后保持暂停。`);pollReady(version,Date.now());return;}
      applyDetail(detail,version);
    } catch(error){if(version===requestVersion){wantsPlay=false;setState('error',`这一集暂时无法播放。点播放按钮重试。${error.message||''}`);}}
  }
  function applyDetail(detail,version){
    if(version!==requestVersion)return;
    if(!detail.browser_audio_ready||!detail.browser_audio_url){wantsPlay=false;setState('error','音频还没有准备完整。点播放按钮重试。');return;}
    current.title=detail.title||current.title;current.duration=detail.duration||current.duration;current.newer=detail.newer;current.older=detail.older;
    const target=Math.min(current.savedPosition,Math.max(0,fullDuration()-.05));
    loadAudio(detail.browser_audio_url,target,wantsPlay);
  }
  async function pollReady(version,started){
    if(version!==requestVersion)return;
    try{
      const detail=await api(`/api/episodes/${encodeURIComponent(current.sid)}/${encodeURIComponent(current.eid)}`);if(version!==requestVersion)return;
      if(detail.browser_audio_ready&&detail.browser_audio_url){applyDetail(detail,version);return;}
      if(detail.status==='failed'||detail.browser_audio_status==='failed'){wantsPlay=false;setState('error','音频准备失败。点播放按钮重试，或者选择其他单集。');return;}
      if(Date.now()-started>15*60*1000){wantsPlay=false;setState('error','音频准备较久。稍后可以点播放按钮重试。');return;}
      if(Number(detail.prepare_eta_seconds)>0)$('player-note').textContent=`正在缓存，预计约 ${Math.ceil(Number(detail.prepare_eta_seconds)/60)} 分钟…`;
    }catch{if(version!==requestVersion)return;$('player-note').textContent='连接暂时中断，正在重新检查。你可以继续浏览。';}
    pollTimer=setTimeout(()=>pollReady(version,started),2500);
  }
  function loadAudio(path,seconds,play){meter.breakContinuity();current.audioUrl=path;pendingSeek=Math.max(0,seconds||0);wantsPlay=play;audio.src=path;audio.load();setState(play?'buffering':'paused',play?'正在连接音频…':'已暂停，点播放按钮继续。');}
  async function playAudio(){const version=requestVersion;try{await audio.play();}catch(error){if(version!==requestVersion||error.name==='AbortError')return;wantsPlay=false;if(error.name==='NotAllowedError')setState('paused','浏览器需要你再点一次播放。点击播放按钮继续即可。');else setState('error','这段音频没能播放。点播放按钮重试。');}}
  function seekTo(seconds){
    if(!current?.audioUrl)return;sampleListening();report(true,state==='playing'?'playing':'paused');meter.breakContinuity();
    const target=Math.max(0,Math.min(Number(seconds)||0,Math.max(0,fullDuration()-.05))),play=wantsPlay&&state!=='finished';
    pendingSeek=null;audio.currentTime=target;
    if(state==='finished')setState('paused','已回到选择的位置，点播放继续。');updateTime();report(true,play?'playing':'paused',true);
  }
  async function toggle(){
    if(!current)return;
    if(['loading','preparing'].includes(state)){wantsPlay=!wantsPlay;setState(state,wantsPlay?'准备完成后会尝试播放。':'已取消自动播放，准备完成后保持暂停。');return;}
    if(['error','restored','finished'].includes(state)){const copy={...current};await startEpisode({id:copy.sid,name:copy.showName,art:copy.art},{id:copy.eid,title:copy.title,duration:copy.duration},{restart:state==='finished',order:copy.order||episodeOrder});return;}
    if(!audio.paused||state==='buffering'){sampleListening();wantsPlay=false;audio.pause();meter.breakContinuity();setState('paused','已暂停。进度正在同步，网页与设备都能接着听。');report(true,'paused');void refreshListening({quiet:true});return;}
    const copy={...current}, previousState=state;
    await startEpisode({id:copy.sid,name:copy.showName,art:copy.art},{id:copy.eid,title:copy.title,duration:copy.duration},{restart:previousState==='finished',order:copy.order||episodeOrder});
  }
  async function adjacent(direction,{automatic=false}={}){
    if(!current)return;
    const next=direction<0?current.newer:current.older;if(!next){if(!automatic)toast(direction<0?'已经是最新一集':'已经是最早一集');return;}
    const show={id:current.sid,name:current.showName,art:current.art},order=current.order;await startEpisode(show,next,{order});
  }
  async function markEpisode(eid,statusValue,button){
    if(!selectedShow)return;button.disabled=true;
    try{const body=await post('/api/listening/mark',{show_id:selectedShow.id,episode_id:eid,status:statusValue});updateProgress(selectedShow.id,eid,body.progress);toast(statusValue==='completed'?'已标记听完，不会增加听时长。':'已标记未听，不会删除实际听时长。');await refreshListening({quiet:true});renderEpisodes();}
    catch(error){toast(error.message||'标记没有保存，请稍后重试。');button.disabled=false;}
  }
  function selectShow(event){const button=event.target.closest('[data-show]');if(!button)return;location.hash='library';episodeFilter='all';renderFilters();loadEpisodes(button.dataset.show,0);}
  function renderFilters(){document.querySelectorAll('[data-filter]').forEach(button=>button.setAttribute('aria-pressed',String(button.dataset.filter===episodeFilter)));}
  $('show-list').onclick=selectShow;$('sidebar-shows').onclick=selectShow;$('show-stats').onclick=selectShow;
  $('manage-shows').onclick=async event=>{const button=event.target.closest('[data-unsub]');if(!button)return;const id=button.dataset.unsub;const name=button.parentElement?.querySelector('span')?.textContent||id;if(!confirm(`退订「${name}」？会删除它的单集缓存、封面和收听进度。`))return;button.disabled=true;button.textContent='正在退订…';try{await api(`/api/sources/${encodeURIComponent(id)}`,{method:'DELETE',timeout:60000});toast('已退订并清理完成。');await loadShows();void loadCachePanel();}catch(error){button.disabled=false;button.textContent='退订';toast('退订失败：'+(error.message||'请稍后重试。'));}};
  $('prefetch-latest').onchange=()=>putSetting('prefetch_latest',Number($('prefetch-latest').value),`已更新：每档自动缓存最新 ${$('prefetch-latest').value} 集，后台正在按新设置补齐。`);
  $('keep-per-show').onchange=()=>putSetting('keep_per_show',Number($('keep-per-show').value),`已更新：清理时每档至少保留最近 ${$('keep-per-show').value} 集。`);
  $('run-cleanup').onclick=async()=>{
    const button=$('run-cleanup'),feedback=$('cache-feedback');button.disabled=true;feedback.textContent='正在按保留规则清理缓存…';
    try{
      await post('/api/maintenance/cleanup',{});
      const finished=await waitMaintenance();
      const result=finished?.result&&finished.result.first!==undefined?finished.result.first:finished?.result;
      if(finished===null){feedback.textContent='清理还在后台执行，稍后重新打开这页查看结果。';}
      else if(finished.error){feedback.textContent='清理失败：'+finished.error;}
      else feedback.textContent=`清理完成：删除 ${Number(result?.deleted)||0} 集，释放 ${gb((Number(result?.freed_mb)||0)*1048576)}。`;
      void loadCachePanel();
    }catch(error){feedback.textContent=error.message||'清理没有完成，请稍后重试。';}
    finally{button.disabled=false;}
  };
  $('clear-cache').onclick=async()=>{
    if(!confirm('清空全部单集缓存？\n\n· 订阅与收听进度不受影响\n· 正在播放、排队或准备中的单集会保留\n· 下次点播会自动重新缓存'))return;
    const button=$('clear-cache'),feedback=$('cache-feedback');button.disabled=true;feedback.textContent='正在清空缓存…';
    try{
      await post('/api/maintenance/clear',{confirm:'clear'});
      const finished=await waitMaintenance();
      if(finished===null){feedback.textContent='清空还在后台执行，稍后重新打开这页查看结果。';}
      else if(finished.error){feedback.textContent='清空失败：'+finished.error;}
      else feedback.textContent=`已清空：删除 ${Number(finished.result?.deleted)||0} 集，释放 ${gb((Number(finished.result?.freed_mb)||0)*1048576)}。`;
      toast('缓存已清空，订阅和收听进度都保留着。');
      await loadShows();void loadCachePanel();
    }catch(error){feedback.textContent=error.message||'清空没有完成，请稍后重试。';}
    finally{button.disabled=false;}
  };
  $('episode-filters').onclick=event=>{const button=event.target.closest('[data-filter]');if(button&&selectedShow){episodeFilter=button.dataset.filter;renderFilters();loadEpisodes(selectedShow.id,0);}};
  $('episode-order').onchange=()=>{episodeOrder=$('episode-order').value;if(selectedShow)loadEpisodes(selectedShow.id,0);};
  $('recent-more').onclick=()=>{recentExpanded=!recentExpanded;renderRecent();};
  $('recent-list').onclick=event=>{if(event.target.closest('[data-retry-sync]')){refreshListening();return;}const button=event.target.closest('[data-recent-episode]');if(!button)return;let e=recent.find(row=>row.show_id===button.dataset.recentShow&&row.episode_id===button.dataset.recentEpisode);if(!e){const record=legacyRecords.get(key(button.dataset.recentShow,button.dataset.recentEpisode));if(record)e={show_id:record.sid,episode_id:record.eid,show_name:record.showName||'历史收听',title:record.title||'历史单集',duration:record.duration||0};}if(e)startEpisode({id:e.show_id,name:e.show_name,art:e.art},{id:e.episode_id,title:e.title,duration:e.duration});};
  $('episode-list').onclick=event=>{if(event.target.closest('[data-retry-list]')){loadEpisodes(selectedShow.id,offset);return;}const mark=event.target.closest('[data-mark-episode]');if(mark){markEpisode(mark.dataset.markEpisode,mark.dataset.mark,mark);return;}const button=event.target.closest('[data-episode]');if(!button||!selectedShow)return;const episode=episodes.find(e=>e.id===button.dataset.episode);if(!episode)return;if(current?.sid===selectedShow.id&&current?.eid===episode.id)toggle();else startEpisode(selectedShow,episode);};
  $('previous-page').onclick=()=>loadEpisodes(selectedShow.id,Math.max(0,offset-PAGE_SIZE));$('next-page').onclick=()=>loadEpisodes(selectedShow.id,offset+PAGE_SIZE);
  $('show-play').onclick=()=>{if(!episodes[0]||!selectedShow)return;const first=episodes[0];if(current?.sid===selectedShow.id&&current?.eid===first.id)toggle();else startEpisode(selectedShow,first);};
  $('toggle-play').onclick=toggle;$('back-15').onclick=()=>seekTo(position()-15);$('forward-15').onclick=()=>seekTo(position()+15);
  $('seek').oninput=()=>{$('elapsed').textContent=clock($('seek').value);};$('seek').onchange=()=>seekTo($('seek').value);
  $('restart').onclick=()=>{if(current)startEpisode({id:current.sid,name:current.showName,art:current.art},{id:current.eid,title:current.title,duration:current.duration},{restart:true});};
  $('newer-episode').onclick=()=>adjacent(current?.order==='oldest'?1:-1);$('older-episode').onclick=()=>adjacent(current?.order==='oldest'?-1:1);
  $('speed').onchange=()=>{sampleListening();report(true,state==='playing'?'playing':'paused');meter.breakContinuity();audio.playbackRate=Number($('speed').value);updateOptionsSummary();};
  $('volume').oninput=()=>{sampleListening();report(true,state==='playing'?'playing':'paused');meter.breakContinuity();audio.volume=Number($('volume').value)/100;$('volume-value').value=$('volume').value;write(KEYS.volume,Number($('volume').value));};
  const storedVolume=read(KEYS.volume,70);$('volume').value=Number.isFinite(storedVolume)?Math.max(0,Math.min(100,storedVolume)):70;$('volume').oninput();
  $('sleep').onchange=()=>{const minutes=Number($('sleep').value);sleepDeadline=minutes?Date.now()+minutes*60000:0;updateOptionsSummary();toast(minutes?`${minutes} 分钟后暂停，同时停止自动连播。`:'睡眠定时已关闭');};
  setInterval(()=>{sampleListening();if(state==='playing')report();if(sleepDeadline&&Date.now()>=sleepDeadline){sleepDeadline=0;$('sleep').value='0';updateOptionsSummary();wantsPlay=false;audio.pause();meter.breakContinuity();if(current&&!['loading','preparing'].includes(state))setState('paused','睡眠定时已到，已暂停并保存进度。');else if(current)setState(state,'睡眠定时已到。音频准备完成后保持暂停。');report(true,'paused');toast('睡眠定时已到，已暂停。');}},500);
  audio.addEventListener('loadedmetadata',()=>{if(!current?.audioUrl)return;if(Number.isFinite(audio.duration)&&audio.duration>0){current.duration=audio.duration;}if(pendingSeek!==null){audio.currentTime=Math.min(pendingSeek,Math.max(0,(audio.duration||pendingSeek)-.01));pendingSeek=null;}meter.breakContinuity();audio.playbackRate=Number($('speed').value);updateTime();if(wantsPlay)playAudio();});
  audio.addEventListener('playing',()=>{if(current&&wantsPlay){meter.breakContinuity();setState('playing','收听中。切换页面、浏览节目都不会打断这一集。');sampleListening();report(true,'playing');}});
  audio.addEventListener('pause',()=>{meter.breakContinuity();if(state==='playing'){wantsPlay=false;setState('paused','已暂停，进度保存在后台。');report(true,'paused');}});
  audio.addEventListener('waiting',()=>{meter.breakContinuity();if(current&&wantsPlay)setState('buffering','正在缓冲。缓冲时间不会计入收听统计。');});
  audio.addEventListener('seeking',()=>meter.breakContinuity());audio.addEventListener('seeked',()=>meter.breakContinuity());
  audio.addEventListener('timeupdate',()=>{sampleListening();updateTime();report();});
  audio.addEventListener('ended',()=>{if(!current)return;sampleListening();meter.breakContinuity();wantsPlay=false;setState('finished','已播放到结尾，收听记录正在同步。没有完整收听的单集仍会保留未听完状态。');report(true,'ended');void refreshListening({quiet:true});if($('autoplay').checked&&(!sleepDeadline||Date.now()<sleepDeadline))void adjacent(current.order==='oldest'?-1:1,{automatic:true});});
  audio.addEventListener('error',()=>{if(current?.audioUrl&&audio.getAttribute('src')){current.savedPosition=position();meter.breakContinuity();wantsPlay=false;setState('error','音频连接中断。点播放按钮重试，会从共享进度继续。');report(true,'paused');}});
  function flushExit(){const event=report(true,'paused');if(event&&navigator.sendBeacon)navigator.sendBeacon('/api/listening/events',new Blob([JSON.stringify(event)],{type:'application/json'}));}
  window.PodcastBeforeLogout = async () => { wantsPlay = false; audio.pause(); report(true,'paused'); await outbox.drain(); };
  window.addEventListener('pagehide',flushExit);document.addEventListener('visibilitychange',()=>{if(document.hidden)report(true,state==='playing'?'playing':'paused');else void refreshListening({quiet:true});});
  $('retry-sync').onclick=async()=>{await outbox.drain();await refreshListening();};$('refresh-stats').onclick=()=>refreshListening();
  setInterval(()=>{void outbox.drain();if(!document.hidden)void refreshListening({quiet:true});},15000);
  $('refresh').onclick=async()=>{
    const button=$('refresh');button.disabled=true;toast('正在更新订阅源，播放会继续。');
    try{let result=await post('/api/refresh',{});if(result.job_id){const expected=result.job_id,until=Date.now()+120000;while(result.status==='running'&&Date.now()<until){await new Promise(resolve=>setTimeout(resolve,1500));result=await api('/api/refresh/status');if(result.job_id!==expected)throw new Error('刷新任务已变化');}if(result.status==='running'){toast('后台仍在更新，已有单集可以继续播放。');return;}if(result.status==='failed')throw new Error(result.error||'刷新失败');result=result.results||{};}
      const failures=Object.entries(result).filter(([,r])=>!r?.ok).map(([id])=>shows.find(s=>s.id===id)?.name||id);await loadShows();toast(failures.length?`其余节目已更新；${failures.join('、')}暂时未更新，可稍后再试。`:'订阅源已更新，新单集会在后台提前准备。');
    }catch{toast('刷新未完成，已有单集仍可浏览和播放。');}finally{button.disabled=false;}
  };
  function updateMigrationBanner(){
    const pending=[...legacyRecords].filter(([k])=>!legacyDone.has(k));$('migration-banner').hidden=migrationDismissed||!pending.length;
    $('migration-message').textContent=`找到 ${pending.length} 条历史进度。导入后，网页与设备可以接着听；不会补记过去的听时长，也不会覆盖后台已有的记录。`;
    $('import-legacy').disabled=importingLegacy;
  }
  async function importLegacyRecords(records,{quiet=false}={}){
    const pending=records.filter(r=>!legacyDone.has(key(r.sid,r.eid)));if(!pending.length)return;
    const entries=pending.map(r=>({show_id:r.sid,episode_id:r.eid,position_ms:Math.max(0,Math.round(Number(r.position||0)*1000)),completed:r.finished===true,updated_at:Math.max(0,Math.floor(Number(r.updated||Date.now())/1000))}));
    let imported=0,skipped=0;const failures=[];
    for(let start=0;start<entries.length;start+=30){
      const body=await post('/api/listening/import',{client_id:clientId,request_id:uuid(),episodes:entries.slice(start,start+30)});
      imported+=Number(body.imported)||0;skipped+=Number(body.skipped)||0;
      for(const result of body.results||[]){const k=key(result.show_id,result.episode_id);if(result.status==='imported'||result.status==='skipped_existing'){legacyDone.add(k);if(result.progress)updateProgress(result.show_id,result.episode_id,result.progress);}else failures.push(k);}
      write(KEYS.migration,[...legacyDone]);
    }
    updateMigrationBanner();renderRecent();
    if(!quiet)toast(failures.length?`已导入 ${imported} 条，后台已有的记录优先。${failures.length} 条对应节目暂时不存在，可以稍后再导入。`:`已导入 ${imported} 条历史进度${skipped?`，跳过 ${skipped} 条后台已有记录`:''}。过去的听时长不会补记。`);
  }
  $('import-legacy').onclick=async()=>{if(importingLegacy)return;importingLegacy=true;updateMigrationBanner();try{await importLegacyRecords([...legacyRecords.values()]);await refreshListening({quiet:true});}catch(error){toast(error.message||'历史进度没有导入，原记录还保留在这台浏览器。');}finally{importingLegacy=false;updateMigrationBanner();}};
  $('dismiss-legacy').onclick=()=>{migrationDismissed=true;updateMigrationBanner();};
  document.querySelectorAll('[data-example-source]').forEach(button=>button.onclick=()=>{if(sourceAdding)return;$('source-url').value=button.dataset.exampleSource;$('source-url').oninput();$('source-form').requestSubmit();$('source-form').scrollIntoView({behavior:matchMedia('(prefers-reduced-motion:reduce)').matches?'auto':'smooth',block:'start'});});
  $('source-search-form').onsubmit=async event=>{
    event.preventDefault();const query=$('source-search').value.trim();if(!query)return;
    const version=++searchVersion;$('search-source').disabled=true;$('source-search-feedback').textContent='正在搜索苹果播客公开目录…';$('source-search-results').hidden=true;
    try{const result=await api('/api/sources/search?q='+encodeURIComponent(query),{timeout:30000});if(version!==searchVersion)return;searchResults=result.results||[];$('source-search-feedback').textContent=searchResults.length?`${result.provider_name||'苹果播客公开目录'} · 找到 ${searchResults.length} 档节目，请核对名称和作者。`:'没有找到节目。试试更完整的名称，或者使用下方的分享链接添加。';$('source-search-results').innerHTML=searchResults.map((show,index)=>`<article class="source-search-result"><img src="${escape(artwork(show.art))}" alt="" loading="lazy" onerror="this.style.visibility='hidden'"><div><h3>${escape(show.name)}</h3><p>${escape(show.author||'播客节目')}${Number(show.episodes)?` · ${Number(show.episodes)} 集`:''}</p></div><button class="quiet-button" data-search-result="${index}">查看节目</button></article>`).join('');$('source-search-results').hidden=!searchResults.length;}
    catch(error){if(version===searchVersion)$('source-search-feedback').textContent=error.message||'搜索暂时无法连接。你仍可以用分享链接添加。';}
    finally{if(version===searchVersion)$('search-source').disabled=false;}
  };
  $('source-search-results').onclick=event=>{const button=event.target.closest('[data-search-result]');if(!button||sourceAdding)return;const result=searchResults[Number(button.dataset.searchResult)];if(!result?.url)return;$('source-url').value=result.url;$('source-url').oninput();$('source-form').requestSubmit();$('source-form').scrollIntoView({behavior:matchMedia('(prefers-reduced-motion:reduce)').matches?'auto':'smooth',block:'start'});};
  $('source-url').oninput=()=>{sourceVersion++;sourcePreview=null;$('source-preview').hidden=true;$('source-feedback').textContent='';};
  $('source-form').onsubmit=async event=>{
    event.preventDefault();if(sourceAdding)return;const entered=$('source-url').value.trim();let parsed;try{const extracted=entered.match(/https?:\/\/[^\s<>"，。；！）】]+/)?.[0]||entered;parsed=new URL(extracted);if(!['https:','http:'].includes(parsed.protocol))throw new Error();}catch{$('source-feedback').textContent='请粘贴以 https:// 或 http:// 开头的节目地址。';return;}
    const version=++sourceVersion;sourcePreview=null;$('source-preview').hidden=true;$('preview-source').disabled=true;$('source-feedback').textContent='正在读取节目，核对订阅地址和最新单集…';
    try{const preview=await post('/api/sources/preview',{url:parsed.href},{timeout:60000});if(version!==sourceVersion)return;sourcePreview=preview;renderSourcePreview(preview);$('source-feedback').textContent=preview.duplicate?'这档节目已经订阅，不会重复添加。':'请确认下面的节目就是你要添加的，再点“确认添加”。';}
    catch(error){if(version!==sourceVersion)return;$('source-feedback').textContent=error.message||'没有找到可订阅的节目。请检查地址，或换用节目提供的订阅地址。';}
    finally{if(version===sourceVersion)$('preview-source').disabled=false;else $('preview-source').disabled=false;}
  };
  function renderSourcePreview(preview){
    const show=preview.show||{},latest=show.latest||{},type={rss:'RSS（播客订阅）',apple:'苹果播客节目链接',xiaoyuzhou:'小宇宙播客链接'}[preview.source_type]||'播客地址';
    $('source-preview').innerHTML=`<div class="preview-identity"><img src="${escape(artwork(show.art))}" alt="${escape(show.name)}节目封面" onerror="this.style.visibility='hidden'"><div><span class="eyebrow">已识别 · ${type}</span><h2 id="source-preview-heading">${escape(show.name)}</h2><p>${escape(show.author||'播客节目')} · ${Number(show.episodes)||0} 集</p></div></div><div class="preview-latest"><span>最新单集</span><strong>${escape(latest.title||'暂无单集')}</strong><p>${date(latest.published||latest.pub_date)} · ${duration(latest.duration)}</p></div>${preview.warning?`<p class="preview-warning">${escape(preview.warning)}</p>`:''}<p class="preview-address">订阅地址 <span>${escape(preview.feed_url||'')}</span></p><div class="preview-footer"><span>确认后会同时出现在网页与设备节目库。</span><button class="show-play" id="confirm-source">${preview.duplicate?'查看已订阅节目':'确认添加'}</button></div>`;
    $('source-preview').hidden=false;$('confirm-source').onclick=confirmSource;
  }
  async function confirmSource(){
    const preview=sourcePreview;if(!preview||sourceAdding)return;
    if(preview.duplicate){location.hash='library';await loadShows();if(preview.existing_show_id)loadEpisodes(preview.existing_show_id,0);return;}
    sourceAdding=true;$('confirm-source').disabled=true;$('source-url').disabled=true;$('source-feedback').textContent='正在添加节目，请稍等…';
    try{const added=await post('/api/sources',{preview_id:preview.preview_id},{timeout:60000});sourcePreview=null;$('source-preview').hidden=true;$('source-url').value='';$('source-feedback').textContent=added.duplicate?'节目已经订阅，没有重复添加。':'添加成功。节目已加入网页与设备共用的节目库。';toast('节目已添加，后台会定期更新新单集。');await loadShows();void loadCachePanel();location.hash='library';if(added.show_id)loadEpisodes(added.show_id,0);}
    catch(error){const expired=error.status===410||(error.status===409&&String(error.message).includes('过期'));$('source-feedback').textContent=expired?'预览已过期，请重新点“查看节目”，核对后再添加。':error.message||'添加没有完成，请重试。';if(expired){sourcePreview=null;$('source-preview').hidden=true;$('source-url').focus();}else if($('confirm-source'))$('confirm-source').disabled=false;}
    finally{sourceAdding=false;$('source-url').disabled=false;}
  }
  function openOptions(open){$('player-options').hidden=!open;$('options-toggle').ariaExpanded=String(open);if(open)$('options-close').focus();else $('options-toggle').focus();}
  $('options-toggle').onclick=()=>openOptions($('player-options').hidden);$('player-detail').onclick=()=>openOptions(true);$('options-close').onclick=()=>openOptions(false);
  document.addEventListener('keydown',event=>{if(event.key==='Escape'&&!$('player-options').hidden)openOptions(false);});document.addEventListener('click',event=>{if(!$('player-options').hidden&&!$('player').contains(event.target))openOptions(false);});
  function view(){if(location.hash==='#discover')history.replaceState(null,'','#sources');const name=location.hash==='#sources'?'sources':location.hash==='#stats'?'stats':location.hash==='#devices'?'devices':'library';['library','sources','stats','devices'].forEach(id=>$(id).hidden=id!==name);document.querySelectorAll('[data-view]').forEach(link=>{if(link.dataset.view===name)link.setAttribute('aria-current','page');else link.removeAttribute('aria-current');});$('context-label').textContent={library:'你的播客收藏',sources:'为收听清单添一档节目',stats:'网页与设备的收听记录',devices:'连接你家的播客机'}[name];if(name==='stats')void refreshListening({quiet:true});if(name==='sources')void loadCachePanel();}
  window.addEventListener('hashchange',view);view();
  const savedTheme=read('portable-podcast-theme','auto');$('theme').value=['auto','light','dark'].includes(savedTheme)?savedTheme:'auto';$('theme').onchange=()=>{document.documentElement.dataset.theme=$('theme').value;write('portable-podcast-theme',$('theme').value);};$('theme').onchange();
  const scrollBehavior=()=>matchMedia('(prefers-reduced-motion:reduce)').matches?'auto':'smooth';$('shelf-back').onclick=()=>$('show-list').scrollBy({left:-330,behavior:scrollBehavior()});$('shelf-forward').onclick=()=>$('show-list').scrollBy({left:330,behavior:scrollBehavior()});
  void loadShows();void refreshListening({quiet:true});void outbox.drain();void loadCachePanel();
})();
