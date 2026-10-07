(() => {
  'use strict';
  const $ = id => document.getElementById(id);
  const install = location.hash.match(/^#install=([0-9a-f]{64})$/);
  if (install) $('setup-key').value = install[1];
  if (location.hash.startsWith('#install=')) history.replaceState(null, '', location.pathname + location.search);
  let initialized = false, busy = false;
  const destination = new URLSearchParams(location.search).get('next');
  const next = destination && /^\/(?:#(?:library|sources|stats|devices))?$/.test(destination) ? destination : '/#devices';
  async function send(path, body) {
    const response = await fetch(path, body ? {method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)} : {});
    const result = await response.json();
    if (!response.ok) throw new Error(result.error || '后台暂时没有完成操作，请稍后再试。');
    return result;
  }
  async function load() {
    $('access-retry').hidden = true; $('access-form').hidden = true;
    try {
      const status = await send('/api/setup/status');
      if (status.authenticated) { location.replace(next); return; }
      initialized = status.initialized;
      $('access-heading').textContent = initialized ? '欢迎回到你的电台' : '给你的电台一个口令';
      $('form-heading').textContent = initialized ? '登录后台' : '首次设置';
      $('access-description').textContent = initialized ? '输入你设置的管理口令。设备已经配对后，会自动连接。' : '只需设置一次。之后在网页生成配对码，就能连接硬件。';
      $('setup-key-label').hidden = initialized;
      $('setup-key').required = !initialized;
      $('admin-password').autocomplete = initialized ? 'current-password' : 'new-password';
      $('password-help').textContent = initialized ? '忘记口令可在自己的后台主机上运行恢复工具。已有收听记录会保留。' : '至少 10 个字符，请妥善保存。硬件配对不需要输入这个口令。';
      $('access-submit').textContent = initialized ? '登录' : '保存并进入';
      if (!initialized && !status.setup_key_available) {
        $('access-feedback').textContent = '后台还没有安装码。请先运行项目提供的安装脚本，然后重新连接。';
        $('access-retry').hidden = false;
        return;
      }
      $('access-form').hidden = false;
      $('access-feedback').textContent = '';
    } catch { $('form-heading').textContent = '后台暂时未连接'; $('access-description').textContent = '检查后台是否已启动，或稍后重试。'; $('access-retry').hidden = false; }
  }
  $('access-retry').onclick = load;
  $('show-password').onclick = () => {
    const visible = $('admin-password').type === 'password';
    $('admin-password').type = visible ? 'text' : 'password';
    $('show-password').textContent = visible ? '隐藏' : '显示';
    $('show-password').ariaPressed = String(visible);
  };
  $('access-form').onsubmit = async event => {
    event.preventDefault(); if (busy) return;
    busy = true; $('access-submit').disabled = true; $('access-feedback').textContent = initialized ? '正在登录…' : '正在保存首次设置…';
    try {
      await send(initialized ? '/api/auth/login' : '/api/setup', {password:$('admin-password').value, ...(!initialized ? {setup_key:$('setup-key').value.trim()} : {})});
      $('setup-key').value = ''; $('admin-password').value = '';
      location.replace(next);
    } catch (error) { $('access-feedback').textContent = error.message; }
    finally { busy = false; $('access-submit').disabled = false; }
  };
  void load();
})();
