/* ===================================================================
 * Popup logic: detect what the active tab is playing, then hand the
 * URL to the local player server and follow the extraction task until
 * the file lands in 「全部曲目」.
 * =================================================================== */
(function () {
  'use strict';

  const $ = (id) => document.getElementById(id);

  const state = {
    tab: null,
    info: null,
    selected: '',
    busy: false,
    poll: 0,
    online: false,
    base: '',
  };

  function setErr(msg) {
    const box = $('err');
    if (!msg) { box.hidden = true; box.textContent = ''; return; }
    box.hidden = false;
    box.textContent = msg;
  }

  function fmtTime(s) {
    const v = Math.max(0, Math.round(s || 0));
    const m = Math.floor(v / 60);
    return m + ':' + String(v % 60).padStart(2, '0');
  }

  function kindOf(c) {
    if (c.kind === 'page') return { label: '整页解析', cls: 'page' };
    if (c.kind === 'meta') return { label: '页面音频', cls: '' };
    if (/\.m3u8(\?|$)/i.test(c.url)) return { label: 'HLS 流', cls: 'hls' };
    if (c.playing) return { label: '正在播放', cls: 'play' };
    if (c.kind === 'element') return { label: '媒体元素', cls: '' };
    return { label: '网络请求', cls: '' };
  }

  /* --------------------------- server status --------------------------- */
  async function checkServer() {
    const node = $('srvState');
    const h = await AE.health();
    state.online = h.ok;
    state.base = h.base;
    node.innerHTML = '';
    const dot = document.createElement('i');
    dot.className = 'dot ' + (h.ok ? 'ok' : 'bad');
    node.appendChild(dot);
    node.appendChild(document.createTextNode(h.ok ? '已连接本地服务' : '未连接（先运行 music_player）'));
    $('btnGo').disabled = !h.ok || !state.info || !state.info.candidates.length;
    if (!h.ok) setErr('无法连接 ' + h.base + '，请先在电脑上启动 music_player。');
    else if ($('err').textContent.indexOf('无法连接') === 0) setErr('');
    return h.ok;
  }

  /* ------------------------------ collect ----------------------------- */
  async function ensureContentScript(tabId) {
    try {
      await chrome.scripting.executeScript({ target: { tabId }, files: ['content.js'] });
    } catch (e) { /* already injected or not allowed */ }
  }

  async function collectFromTab(tabId) {
    try {
      return await chrome.tabs.sendMessage(tabId, { type: 'ae-collect' });
    } catch (e) {
      return null;
    }
  }

  async function loadTab() {
    const hint = $('listHint');
    const box = $('cands');
    box.innerHTML = '';
    state.info = null;
    state.selected = '';
    hint.hidden = false;
    hint.textContent = '正在检测当前页面…';

    let info = await collectFromTab(state.tab.id);
    if (!info) {
      await ensureContentScript(state.tab.id);
      info = await collectFromTab(state.tab.id);
    }
    if (!info || !info.ok) {
      hint.textContent = info && info.error
        ? '无法读取该页面：' + info.error
        : '这个页面不允许读取（浏览器设置页等特殊页面），请切到正在放歌的网页再试。';
      $('btnGo').disabled = true;
      return;
    }
    state.info = info;
    renderCands();
  }

  function renderCands() {
    const hint = $('listHint');
    const box = $('cands');
    const info = state.info;
    const list = (info.candidates || []).filter((c) => c.usable !== false);
    box.innerHTML = '';

    $('tabTitle').textContent = info.pageTitle || info.host || '当前标签';
    $('tabSub').textContent = (info.host || '') + (info.hasPlaying ? ' · 检测到正在播放' : '');

    if (!list.length) {
      hint.textContent = '没有检测到可抓取的音源，可以直接到播放器的「在线提取」里粘贴链接。';
      $('btnGo').disabled = true;
      return;
    }
    hint.hidden = true;

    // Prefer something that is genuinely playing, else the biggest payload.
    const preferred = list.find((c) => c.playing) || list[0];
    if (!list.some((c) => c.url === state.selected)) state.selected = preferred.url;

    list.forEach((c) => {
      const k = kindOf(c);
      const row = document.createElement('label');
      row.className = 'cand' + (c.url === state.selected ? ' is-on' : '');
      row.dataset.url = c.url;
      const subs = [];
      subs.push('<span class="tag ' + k.cls + '">' + k.label + '</span>');
      if (c.duration) subs.push('<span>' + fmtTime(c.duration) + '</span>');
      if (c.note) subs.push('<span>' + escapeHtml(c.note) + '</span>');
      row.innerHTML =
        '<input type="radio" name="src"' + (c.url === state.selected ? ' checked' : '') + '>' +
        '<div class="cand-main">' +
          '<div class="cand-title">' + escapeHtml(c.title || c.url) + '</div>' +
          '<div class="cand-sub">' + subs.join('') + '</div>' +
          '<div class="cand-url">' + escapeHtml(c.url) + '</div>' +
        '</div>';
      row.addEventListener('click', () => {
        state.selected = c.url;
        box.querySelectorAll('.cand').forEach((n) => n.classList.toggle('is-on', n.dataset.url === c.url));
        row.querySelector('input').checked = true;
        suggestName();
      });
      box.appendChild(row);
    });

    $('btnGo').disabled = state.busy || !state.online;
    suggestName();
  }

  function escapeHtml(s) {
    return String(s).replace(/[&<>"']/g, (ch) => (
      { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[ch]
    ));
  }

  function suggestName() {
    const c = (state.info.candidates || []).find((x) => x.url === state.selected);
    const input = $('name');
    if (input.dataset.dirty === '1') return;
    const raw = state.info.ogTitle || state.info.pageTitle || '';
    input.value = AE.suggestName(raw, state.selected);
  }

  /* ------------------------------ extract ----------------------------- */
  function setProgress(pct, label) {
    $('prog').hidden = false;
    $('barFill').style.width = Math.max(2, Math.min(100, pct * 100)).toFixed(1) + '%';
    $('progLabel').textContent = label;
  }

  async function start() {
    if (state.busy) return;
    if (!state.selected) { setErr('请先选择一个音源'); return; }
    setErr('');
    state.busy = true;
    $('btnGo').disabled = true;
    $('btnGo').textContent = '提取中…';
    $('foot').hidden = true;
    setProgress(0.01, '提交任务…');

    const s = await AE.settings();
    try {
      const res = await AE.extract({
        url: state.selected,
        output: $('name').value.trim(),
        format: s.format,
        bitrate: Number(s.bitrate),
      });
      poll(res.taskId);
    } catch (e) {
      finish(null, e.message || String(e));
    }
  }

  function poll(taskId) {
    let tries = 0;
    clearInterval(state.poll);
    state.poll = setInterval(async () => {
      tries += 1;
      if (tries > 600) { clearInterval(state.poll); finish(null, '任务超时'); return; }
      let t;
      try {
        t = await AE.findTask(taskId);
      } catch (e) {
        return; // transient network blip; retry next tick
      }
      if (!t) { setProgress(0.02, '任务已结束'); return; }
      if (t.status === 'running') {
        setProgress(Math.max(0.03, t.progress || 0), AE.stageLabel(t.stage) + percent(t));
      } else if (t.status === 'queued') {
        setProgress(0.02, '排队中…');
      } else {
        clearInterval(state.poll);
        finish(t, t.status === 'done' ? '' : (t.error || '提取失败'));
      }
    }, 800);
  }

  function percent(t) { return ' · ' + Math.round((t.progress || 0) * 100) + '%'; }

  function finish(task, err) {
    clearInterval(state.poll);
    state.busy = false;
    $('btnGo').disabled = false;
    $('btnGo').textContent = '提取到全部曲目';
    if (err) {
      setErr(err);
      setProgress(task && task.progress ? task.progress : 0, '失败');
      return;
    }
    setProgress(1, '完成，已加入曲库');
    $('foot').hidden = false;
    $('footText').textContent = '已保存到曲库：' + (task.output || task.title || '');
  }

  /* ------------------------------- wiring ----------------------------- */
  async function boot() {
    const s = await AE.settings();
    $('cfgUrl').value = s.serverUrl;
    $('cfgFormat').value = s.format;
    $('cfgBitrate').value = String(s.bitrate);

    const tabs = await chrome.tabs.query({ active: true, currentWindow: true });
    state.tab = tabs && tabs[0];
    if (!state.tab) {
      $('listHint').textContent = '没有活动标签页';
      return;
    }
    await loadTab();
    await checkServer();
  }

  $('btnRescan').addEventListener('click', () => { if (state.tab) loadTab().then(checkServer); });
  $('btnGo').addEventListener('click', start);
  $('name').addEventListener('input', () => { $('name').dataset.dirty = '1'; });

  $('btnSet').addEventListener('click', () => {
    const p = $('setPanel');
    p.hidden = !p.hidden;
    if (!p.hidden) $('cfgUrl').focus();
  });
  $('btnOpen').addEventListener('click', async () => {
    const s = await AE.settings();
    chrome.tabs.create({ url: s.serverUrl.replace(/\/+$/, '') + '/' });
  });
  $('btnLib').addEventListener('click', async () => {
    const s = await AE.settings();
    chrome.tabs.create({ url: s.serverUrl.replace(/\/+$/, '') + '/' });
  });

  $('cfgSave').addEventListener('click', async () => {
    const next = await AE.save({
      serverUrl: $('cfgUrl').value.trim(),
      format: $('cfgFormat').value,
      bitrate: Number($('cfgBitrate').value),
    });
    $('cfgUrl').value = next.serverUrl;
    await checkServer();
    $('setPanel').hidden = true;
  });
  $('cfgReset').addEventListener('click', async () => {
    const next = await AE.save({ serverUrl: AE.DEFAULTS.serverUrl, format: AE.DEFAULTS.format, bitrate: AE.DEFAULTS.bitrate });
    $('cfgUrl').value = next.serverUrl;
    $('cfgFormat').value = next.format;
    $('cfgBitrate').value = String(next.bitrate);
    await checkServer();
  });

  boot();
})();
