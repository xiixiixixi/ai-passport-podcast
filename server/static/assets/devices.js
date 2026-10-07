(() => {
  'use strict';
  const $ = id => document.getElementById(id);
  const escape = value => String(value ?? '').replace(/[&<>"']/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
  let pairing = null, refreshing = false, waitingDevice = null;
  function feedback(message) { $('device-feedback').textContent = message; }
  async function request(path, method = 'GET', body) {
    const controller = new AbortController(), timer = setTimeout(() => controller.abort(), 20000);
    try {
      const response = await fetch(path, {method, signal:controller.signal, ...(body !== undefined ? {headers:{'Content-Type':'application/json'},body:JSON.stringify(body)} : {})});
      if (response.status === 401) location.replace('/login?next=%2F%23devices');
      const result = await response.json();
      if (!response.ok) throw new Error(result.error || '后台暂时没有完成操作，请稍后再试。');
      return result;
    } finally { clearTimeout(timer); }
  }
  function seen(timestamp) { return timestamp ? new Date(timestamp * 1000).toLocaleString('zh-CN', {month:'numeric',day:'numeric',hour:'2-digit',minute:'2-digit'}) : '还没有连接'; }
  async function load({quiet = false} = {}) {
    if (refreshing) return; refreshing = true;
    try {
      const data = await request('/api/devices');
      const devices = data.devices || [], active = devices.filter(row => !row.revoked);
      $('device-count').textContent = `已授权 ${active.length} 台`;
      $('device-server-url').value = data.server_url || '';
      if (/^https?:\/\/(?:localhost|127\.|\[::1\])/.test(data.server_url || '')) $('server-url-help').textContent = '当前是本机地址，硬件无法使用。请用后台主机的局域网地址打开本网页，或在安装时设置后台公开地址。';
      if (!document.activeElement?.closest('.device-rename-form')) {
        $('device-list').innerHTML = devices.length ? devices.map(row => `<article class="device-row" data-device-id="${escape(row.id)}"><div><h3>${escape(row.name)} <span class="device-status">${row.revoked ? '已移除' : row.pending ? '等待新设置连接' : '已授权'}</span></h3><p>${escape(row.id)} · 最近连接 ${seen(row.last_seen)}</p><form class="device-rename-form" hidden><label>设备名称 <input name="name" maxlength="40" value="${escape(row.name)}" required></label><button class="quiet-button" type="submit">保存名称</button><button class="quiet-button" type="button" data-cancel-rename>取消</button></form></div><div class="device-row-actions"><button class="quiet-button" data-rename>改名</button>${!row.revoked ? '<button class="quiet-button" data-revoke>移除设备</button>' : ''}</div></article>`).join('') : '<p class="empty">还没有连接设备。生成一个配对码，把第一台播客机连进来。</p>';
      }
      $('device-list').ariaBusy = 'false';
      if (pairing && data.pairing?.claimed_device_id) {
        const device = devices.find(row => row.id === data.pairing.claimed_device_id);
        if (device) {
          pairing = null; $('pair-code').textContent = ''; $('pair-details').hidden = true;
          waitingDevice = device.id;
          feedback('配对码已提交，硬件保存设置后会自动连接。');
        }
      }
      if (waitingDevice) {
        const device = devices.find(row => row.id === waitingDevice);
        if (device && !device.pending && !device.revoked) { waitingDevice = null; feedback(`“${device.name}”已经连接，可以在硬件上选择节目收听。`); }
      }
    } catch (error) { if (!quiet) feedback(error.message || '暂时未连接后台，请稍后更新设备。'); }
    finally { refreshing = false; }
  }
  $('refresh-devices').onclick = () => load();
  $('create-pairing').onclick = async () => {
    const button = $('create-pairing'); button.disabled = true; feedback('正在生成配对码…');
    try {
      pairing = await request('/api/devices/pairings', 'POST', {});
      $('pair-code').textContent = pairing.code; $('pair-details').hidden = false;
      $('device-server-url').value = pairing.server_url;
      feedback('配对码只给这次设备设置使用；生成新码会让上一个码失效。');
      countdown();
    } catch (error) { feedback(error.message || '没有生成配对码，请重试。'); }
    finally { button.disabled = false; }
  };
  function countdown() {
    if (!pairing) return;
    const seconds = Math.max(0, pairing.expires_at - Math.floor(Date.now() / 1000));
    $('pair-countdown').textContent = seconds ? `${Math.floor(seconds / 60)} 分 ${String(seconds % 60).padStart(2, '0')} 秒内有效，只能使用一次。` : '配对码已过期，需要时再生成一个。';
    if (!seconds) { pairing = null; $('pair-code').textContent = '已过期'; }
  }
  async function copy(value, element) {
    if (!value) return;
    try {
      if (!navigator.clipboard) throw new Error();
      await navigator.clipboard.writeText(value); feedback('已复制。');
    } catch {
      if (element.tagName === 'INPUT') element.select();
      else { const range = document.createRange(); range.selectNodeContents(element); const selection = window.getSelection(); selection.removeAllRanges(); selection.addRange(range); }
      feedback('已选中文字。使用手机的复制菜单，或电脑的复制快捷键。');
    }
  }
  $('copy-pair-code').onclick = () => { if (pairing) void copy(pairing.code, $('pair-code')); };
  $('copy-server-url').onclick = () => copy($('device-server-url').value, $('device-server-url'));
  $('device-list').onclick = async event => {
    const row = event.target.closest('[data-device-id]'); if (!row) return;
    const form = row.querySelector('form');
    if (event.target.closest('[data-rename]')) { form.hidden = false; form.elements.name.focus(); return; }
    if (event.target.closest('[data-cancel-rename]')) { form.hidden = true; return; }
    const button = event.target.closest('[data-revoke]');
    if (!button) return;
    if (button.dataset.confirm !== 'yes') { button.dataset.confirm = 'yes'; button.textContent = '确认移除'; feedback('这台设备将需要重新配对。已有收听记录会保留；继续点击“确认移除”完成。'); return; }
    button.disabled = true;
    try { await request('/api/devices/' + encodeURIComponent(row.dataset.deviceId), 'DELETE'); feedback('设备权限已移除，收听记录仍保留。'); await load(); }
    catch (error) { feedback(error.message); button.disabled = false; }
  };
  $('device-list').addEventListener('submit', async event => {
    const form = event.target.closest('.device-rename-form'); if (!form) return;
    event.preventDefault(); const row = form.closest('[data-device-id]'), button = form.querySelector('[type="submit"]'); button.disabled = true;
    try { await request('/api/devices/' + encodeURIComponent(row.dataset.deviceId), 'PATCH', {name:form.elements.name.value}); form.hidden = true; button.blur(); feedback('设备名称已保存。'); await load(); }
    catch (error) { feedback(error.message); button.disabled = false; }
  });
  $('admin-logout').onclick = async () => {
    const button = $('admin-logout'); button.disabled = true;
    try { await window.PodcastBeforeLogout?.(); await request('/api/auth/logout', 'POST', {}); location.replace('/login'); }
    catch (error) { feedback(error.message || '退出登录暂时没有完成，请重试。'); button.disabled = false; }
  };
  const enter = () => { if (location.hash === '#devices') void load(); };
  window.addEventListener('hashchange', enter); enter();
  setInterval(countdown, 1000);
  setInterval(() => { if (!document.hidden && location.hash === '#devices') void load({quiet:true}); }, 5000);
})();
