/* Audio Extractor · 本地音乐播放器
 * 原生 JS，零外部依赖。后端 API 见 src/server/player_server.cpp
 */
'use strict';

// ============================================================== state
const state = {
  tracks: [],        // 全部曲目
  view: [],          // 过滤 + 排序后的列表（播放队列来源）
  currentId: null,
  sortKey: localStorage.getItem('ae_sort') || 'recent',
  theme: localStorage.getItem('ae_theme') || 'dark',
  repeat: localStorage.getItem('ae_repeat') || 'off',   // off | all | one
  shuffle: localStorage.getItem('ae_shuffle') === '1',
  order: [],         // view 下标的播放顺序
  orderPos: -1,
  queue: [],         // 「下一首播放」插队队列（track id）
  tasks: [],
  searching: false,
  ctxTarget: null,
  browsePath: '',
  category: { kind: 'all', value: '' },   // all | fav | album | artist | playlist
  playlists: [],
  addToTarget: null,                      // track id currently being filed
};

const el = (id) => document.getElementById(id);
const audio = el('audio');
const icon = (svgEl, name) => { const u = svgEl.querySelector('use'); if (u) u.setAttribute('href', '#i-' + name); };

// ============================================================== utils
function fmtTime(sec) {
  if (!isFinite(sec) || sec < 0) sec = 0;
  const s = Math.floor(sec % 60);
  const m = Math.floor((sec / 60) % 60);
  const h = Math.floor(sec / 3600);
  const p = (n) => String(n).padStart(2, '0');
  return h > 0 ? `${h}:${p(m)}:${p(s)}` : `${m}:${p(s)}`;
}

function fmtDuration(sec) {
  const h = Math.floor(sec / 3600), m = Math.floor((sec % 3600) / 60);
  if (h > 0) return `${h} 小时 ${m} 分`;
  if (m > 0) return `${m} 分钟`;
  return `${Math.floor(sec)} 秒`;
}

function fmtSize(bytes) {
  const gb = bytes / (1024 ** 3), mb = bytes / (1024 ** 2);
  if (gb >= 1) return gb.toFixed(2) + ' GB';
  if (mb >= 1) return mb.toFixed(1) + ' MB';
  return Math.max(1, Math.round(bytes / 1024)) + ' KB';
}

function esc(s) {
  return String(s == null ? '' : s).replace(/[&<>"']/g, (c) =>
    ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
}

let toastTimer = null;
function toast(msg) {
  const t = el('toast');
  t.textContent = msg;
  t.classList.add('show');
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => t.classList.remove('show'), 2600);
}

async function api(path, opts) {
  const res = await fetch(path, opts);
  if (!res.ok) {
    let detail = '';
    try { detail = (await res.json()).error || ''; } catch (e) { /* ignore */ }
    throw new Error(detail || `HTTP ${res.status}`);
  }
  return res.json();
}

const cmpText = (a, b) => String(a || '').localeCompare(String(b || ''), 'zh-Hans-CN');

// ==================================================== playlist categories
const CAT_UNKNOWN = { album: '未知专辑', artist: '未知歌手' };

// 自己提取出来的文件常常没有 ID3 标签，这时用常见的「歌名 - 歌手」命名习惯
// 补出歌手，否则「按歌手」分类只剩一个「未知歌手」。("../视频标题 - P88 088. 岁月
// 神偷 - 金玟岐" 这类命名取最后一段即可。)
function deriveArtist(t) {
  if ((t.artist || '').trim()) return t.artist.trim();
  const src = String(t.title || t.fileName || '')
    .replace(/\.(mp3|wav|flac|m4a|aac|ogg|opus)$/i, '');
  const parts = src.split(/\s*[-–—－]\s*/).filter((s) => s.trim());
  if (parts.length >= 2) {
    const last = parts[parts.length - 1].trim();
    if (last && last.length <= 20 && !/\d{2,}/.test(last)) return last;
  }
  return '';
}

function loadCategoryPref() {
  try {
    const raw = localStorage.getItem('ae_cat');
    if (!raw) return { kind: 'all', value: '' };
    const p = JSON.parse(raw);
    return (p && p.kind) ? { kind: p.kind, value: p.value || '' } : { kind: 'all', value: '' };
  } catch (e) { return { kind: 'all', value: '' }; }
}

function saveCategoryPref() {
  try { localStorage.setItem('ae_cat', JSON.stringify(state.category)); } catch (e) { /* ignore */ }
}

const catValue = (t, field) => ((t[field] || '').trim() || CAT_UNKNOWN[field] || '未知');

// Group the library by a metadata field -> [{name, count}], biggest first.
function groupBy(field) {
  const map = new Map();
  state.tracks.forEach((t) => map.set(catValue(t, field), (map.get(catValue(t, field)) || 0) + 1));
  return [...map.entries()]
    .map(([name, count]) => ({ name, count }))
    .sort((a, b) => b.count - a.count || cmpText(a.name, b.name));
}

function updateCatChip() {
  const c = state.category;
  const chip = el('catChip');
  if (c.kind === 'all') { chip.hidden = true; return; }
  chip.hidden = false;
  const pl = c.kind === 'playlist' ? state.playlists.find((x) => x.id === c.value) : null;
  el('catChipText').textContent =
    c.kind === 'fav' ? '我的收藏'
    : c.kind === 'playlist' ? (pl ? pl.name : '（列表已删除）')
    : c.value;
  icon(el('catChipIco'),
    c.kind === 'fav' ? 'heart' : c.kind === 'playlist' ? 'list' : c.kind === 'album' ? 'disc' : 'user');
}

function renderCategories() {
  el('catNumAll').textContent = state.tracks.length;
  el('catNumFav').textContent = state.tracks.filter((t) => t.favorite).length;

  const paint = (host, field, kind) => {
    const rows = groupBy(field);
    host.innerHTML = rows.length
      ? rows.map((r) => `
          <button class="cat-item" data-cat="${kind}" data-value="${esc(r.name)}" title="${esc(r.name)}">
            <svg class="icon"><use href="#i-${kind === 'album' ? 'disc' : 'user'}"/></svg>
            <span class="cat-name">${esc(r.name)}</span><em class="cat-num">${r.count}</em>
          </button>`).join('')
      : '<div class="cat-empty">暂无</div>';
  };
  paint(el('catAlbums'), 'album', 'album');
  paint(el('catArtists'), 'artist', 'artist');

  const cur = state.category;
  const host = el('catPlaylists');
  host.innerHTML = state.playlists.length
    ? state.playlists.map((p) => `
        <div class="cat-wrap ${cur.kind === 'playlist' && cur.value === p.id ? 'is-active' : ''}">
          <button class="cat-item" data-cat="playlist" data-value="${esc(p.id)}" title="${esc(p.name)}">
            <svg class="icon"><use href="#i-list"/></svg>
            <span class="cat-name">${esc(p.name)}</span><em class="cat-num">${p.ids.length}</em>
          </button>
          <button class="cat-del" data-delpl="${esc(p.id)}" title="删除列表「${esc(p.name)}」">
            <svg class="icon"><use href="#i-trash"/></svg>
          </button>
        </div>`).join('')
    : '';
  host.querySelectorAll('[data-delpl]').forEach((b) => {
    b.addEventListener('click', (ev) => { ev.stopPropagation(); removePlaylist(b.dataset.delpl); });
  });

  document.querySelectorAll('.cat-item').forEach((b) => {
    b.classList.toggle('is-active',
      b.dataset.cat === state.category.kind && (b.dataset.value || '') === state.category.value);
  });
  document.querySelectorAll('.cat-wrap').forEach((w) => {
    w.classList.toggle('is-active', !!w.querySelector('.cat-item.is-active'));
  });
  updateCatChip();
}

// ==================================================== user playlists
const post = (path, body) => api(path, {
  method: 'POST', headers: { 'Content-Type': 'application/json' },
  body: JSON.stringify(body || {}),
});

async function loadPlaylists() {
  try {
    const data = await api('/api/playlists');
    state.playlists = data.playlists || [];
  } catch (e) { state.playlists = state.playlists || []; }
  renderCategories();
  if (state.category.kind === 'playlist') applyFilterSort();
}

async function createPlaylist(name) {
  try {
    const r = await post('/api/playlists', { name: name.trim() });
    state.playlists.push(r.playlist);
    renderCategories();
    toast('已创建列表「' + r.playlist.name + '」');
    return r.playlist;
  } catch (e) {
    toast('创建失败：' + e.message);
    return null;
  }
}

async function removePlaylist(id) {
  const p = state.playlists.find((x) => x.id === id);
  if (!p) return;
  if (!confirm('删除列表「' + p.name + '」？其中的歌曲不会被移除，只是列表消失。')) return;
  try {
    await api('/api/playlists?id=' + encodeURIComponent(id), { method: 'DELETE' });
    state.playlists = state.playlists.filter((x) => x.id !== id);
    if (state.category.kind === 'playlist' && state.category.value === id) setCategory('all', '');
    else { renderCategories(); applyFilterSort(); }
    toast('已删除列表');
  } catch (e) { toast('删除失败：' + e.message); }
}

async function toggleInPlaylist(playlistId, trackId) {
  const p = state.playlists.find((x) => x.id === playlistId);
  if (!p) return;
  const has = p.ids.includes(trackId);
  const nextIds = has ? p.ids.filter((i) => i !== trackId) : p.ids.concat(trackId);
  const snapshot = p.ids.slice();
  p.ids = nextIds;                                   // optimistic
  renderCategories(); renderAddToList(); applyFilterSort();
  try {
    const r = await post('/api/playlists/tracks', { playlistId, trackId });
    p.ids = p.ids.filter((i) => i !== trackId);
    if (r.added) p.ids.push(trackId);
  } catch (e) {
    p.ids = snapshot;
    toast('操作失败：' + e.message);
  }
  renderCategories(); renderAddToList(); applyFilterSort();
}

// ---------- 新建列表弹窗 ----------
function openPlModal() {
  el('plModal').hidden = false;
  el('plName').value = '';
  setTimeout(() => el('plName').focus(), 40);
}
function closePlModal() { el('plModal').hidden = true; }

el('addPlaylistBtn').addEventListener('click', (ev) => { ev.stopPropagation(); openPlModal(); });
el('plClose').addEventListener('click', closePlModal);
el('plCancel').addEventListener('click', closePlModal);
el('plModal').addEventListener('pointerdown', (ev) => { if (ev.target === el('plModal')) closePlModal(); });
el('plCreate').addEventListener('click', async () => {
  const v = el('plName').value.trim();
  if (!v) { toast('先给列表起个名字'); return; }
  const created = await createPlaylist(v);
  closePlModal();
  if (created && state.addToTarget) renderAddToList();
});
el('plName').addEventListener('keydown', (ev) => {
  if (ev.key === 'Enter') { ev.preventDefault(); el('plCreate').click(); }
});

// ---------- 添加到列表弹窗 ----------
function renderAddToList() {
  const box = el('addToList');
  const id = state.addToTarget;
  const t = state.tracks.find((x) => x.id === id);
  el('addToInfo').textContent = t ? t.title : '—';
  if (!state.playlists.length) {
    box.innerHTML = '<div class="modal-loading">还没有列表，点右下角「新建列表」创建一个</div>';
    return;
  }
  box.innerHTML = state.playlists.map((p) => {
    const checked = p.ids.includes(id);
    return `
      <button class="pl-row ${checked ? 'is-on' : ''}" data-pl="${esc(p.id)}">
        <span class="pl-box"><svg class="icon"><use href="#i-${checked ? 'check' : 'plus'}"/></svg></span>
        <span class="pl-row-name">${esc(p.name)}</span>
        <em class="cat-num">${p.ids.length}</em>
      </button>`;
  }).join('');
  box.querySelectorAll('[data-pl]').forEach((b) => {
    b.addEventListener('click', () => toggleInPlaylist(b.dataset.pl, id));
  });
}

function openAddToList(id) {
  state.addToTarget = id;
  el('addToModal').hidden = false;
  renderAddToList();
}
function closeAddToList() { el('addToModal').hidden = true; state.addToTarget = null; }

el('addToClose').addEventListener('click', closeAddToList);
el('addToDone').addEventListener('click', closeAddToList);
el('addToNew').addEventListener('click', openPlModal);
el('addToModal').addEventListener('pointerdown', (ev) => {
  if (ev.target === el('addToModal')) closeAddToList();
});

function setCategory(kind, value) {
  state.category = { kind, value: value || '' };
  saveCategoryPref();
  renderCategories();
  applyFilterSort();
  switchTo('library');
}

document.addEventListener('click', (ev) => {
  const it = ev.target.closest('.cat-item');
  if (!it) return;
  setCategory(it.dataset.cat, it.dataset.value || '');
});

el('catChipClear').addEventListener('click', () => setCategory('all', ''));
el('emptyCatBack').addEventListener('click', () => setCategory('all', ''));

async function toggleFavorite(id) {
  const t = state.tracks.find((x) => x.id === id);
  if (!t) return;
  const next = !t.favorite;
  t.favorite = next;                       // optimistic update
  applyFilterSort();
  renderCategories();
  try {
    const r = await api('/api/favorites', {
      method: 'POST', headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ id, on: next }),
    });
    t.favorite = !!r.favorite;
    toast(next ? '已加入收藏' : '已取消收藏');
  } catch (e) {
    t.favorite = !next;
    toast('收藏失败：' + e.message);
  }
  applyFilterSort();
  renderCategories();
}

// ============================================================== theme
function applyTheme(t) {
  state.theme = t;
  document.documentElement.dataset.theme = t;
  el('themeLabel').textContent = t === 'dark' ? '深色' : '浅色';
  icon(el('themeIcon'), t === 'dark' ? 'moon' : 'sun');
  localStorage.setItem('ae_theme', t);
}

// ========================================================== LAN access
const FW_HINT = {
  ok: '防火墙入站规则已放行',
  'not-supported': '本机无法自动放行防火墙',
  'needs-admin': '需要管理员权限才能放行端口',
  failed: '防火墙放行失败',
  skipped: '未对本机外部开放或已跳过',
};

function closeLanModal() { el('lanModal').hidden = true; }

async function copyToClipboard(text) {
  try {
    if (navigator.clipboard && window.isSecureContext) {
      await navigator.clipboard.writeText(text);
      return true;
    }
  } catch (e) { /* fall through to the legacy path */ }
  try {
    const ta = document.createElement('textarea');
    ta.value = text;
    ta.style.position = 'fixed';
    ta.style.opacity = '0';
    document.body.appendChild(ta);
    ta.select();
    const ok = document.execCommand('copy');
    document.body.removeChild(ta);
    return ok;
  } catch (e) { return false; }
}

async function loadNetInfo() {
  const box = el('lanList');
  box.innerHTML = '<div class="modal-loading">载入中…</div>';
  let net;
  try {
    net = await api('/api/net');
  } catch (e) {
    box.innerHTML = '<div class="modal-loading">获取网络信息失败：' + esc(e.message) + '</div>';
    el('lanInfo').textContent = '—';
    return;
  }

  const urls = net.urls || [];
  const lanUrls = urls.slice(1);
  const rows = urls.map((u, i) => {
    const isLocal = u.includes('127.0.0.1') || u.includes('localhost');
    return `
      <div class="net-item">
        <div class="net-ico"><svg class="icon"><use href="#i-${isLocal ? 'window' : 'lan'}"/></svg></div>
        <div class="net-main">
          <div class="net-url" title="${esc(u)}">${esc(u)}</div>
          <div class="net-hint">${isLocal ? '这台电脑上打开' : '同一 Wi-Fi / 网线的手机、平板、其它电脑也能打开'}</div>
        </div>
        ${isLocal ? '' : `<button class="btn btn-ghost btn-sm" data-copy="${esc(u)}">复制</button>`}
      </div>`;
  }).join('');

  const fw = net.firewall || 'skipped';
  const fwNote = `<div class="net-note ${fw === 'ok' ? 'is-ok' : (fw === 'skipped' ? '' : 'is-warn')}">
      <svg class="icon"><use href="#i-${fw === 'ok' ? 'check' : 'info'}"/></svg>
      <span>${esc(FW_HINT[fw] || fw)}${net.firewallHint ? '：' + esc(net.firewallHint) : ''}</span>
    </div>`;

  const tip = lanUrls.length
    ? '把上面的地址发给同一局域网的设备即可直接播放这台机器上的音乐'
    : '没检测到局域网地址：请确认电脑已连上 Wi-Fi / 网线，或与手机处于同一网络';

  box.innerHTML = rows + `<div class="net-tip">${esc(tip)}</div>` + fwNote;
  el('lanInfo').textContent =
    `${net.hostName ? net.hostName + ' · ' : ''}端口 ${net.port}${net.lanEnabled ? ' · 已对局域网开放' : ' · 仅本机'}`;

  box.querySelectorAll('[data-copy]').forEach((b) => {
    b.addEventListener('click', async () => {
      const ok = await copyToClipboard(b.dataset.copy);
      toast(ok ? '已复制：' + b.dataset.copy : '复制失败，请手动选择地址');
    });
  });
}

el('lanBtn').addEventListener('click', () => { el('lanModal').hidden = false; loadNetInfo(); });
el('lanRefresh').addEventListener('click', loadNetInfo);
el('lanClose').addEventListener('click', closeLanModal);
el('lanCancel').addEventListener('click', closeLanModal);
el('lanModal').addEventListener('pointerdown', (ev) => {
  if (ev.target === el('lanModal')) closeLanModal();
});

// ---------------------------------------------- browser extension modal
function closeExtModal() { el('extModal').hidden = true; }

async function loadExtInfo() {
  const st = el('extStatus');
  try {
    const info = await api('/api/extension/info');
    el('extDir').textContent = info.dir;
    st.textContent = info.available
      ? '✓ 扩展文件就绪，按下面 4 步安装即可'
      : '✗ 未找到扩展文件，请重新构建项目（extension 目录会随构建同步）';
    st.classList.toggle('is-ok', !!info.available);
    st.classList.toggle('is-bad', !info.available);
    el('extInfo').textContent = '服务地址 ' + info.serverUrl;
  } catch (e) {
    st.textContent = '获取扩展信息失败：' + e.message;
  }
}

el('extBtn').addEventListener('click', () => { el('extModal').hidden = false; loadExtInfo(); });
el('extClose').addEventListener('click', closeExtModal);
el('extCancel').addEventListener('click', closeExtModal);
el('extModal').addEventListener('pointerdown', (ev) => {
  if (ev.target === el('extModal')) closeExtModal();
});
el('extCopy').addEventListener('click', async () => {
  const ok = await copyToClipboard(el('extDir').textContent);
  toast(ok ? '已复制扩展目录路径' : '复制失败，请手动选择该路径');
});

// ---------------------------------------------- delete confirmation
el('delClose').addEventListener('click', closeDeleteDialog);
el('delCancel').addEventListener('click', closeDeleteDialog);
el('delConfirm').addEventListener('click', confirmDelete);
el('delModal').addEventListener('pointerdown', (ev) => {
  if (ev.target === el('delModal')) closeDeleteDialog();
});

// ----------------------------------------------- sidebar drawer (narrow)
const appEl = document.querySelector('.app');
function openDrawer() {
  if (window.matchMedia('(min-width: 861px)').matches) return;
  appEl.classList.add('is-drawer');
  el('drawerBackdrop').hidden = false;
}
function closeDrawer() {
  appEl.classList.remove('is-drawer');
  el('drawerBackdrop').hidden = true;
}
el('menuFab').addEventListener('click', openDrawer);
el('drawerBackdrop').addEventListener('click', closeDrawer);
window.addEventListener('resize', () => {
  if (window.matchMedia('(min-width: 861px)').matches) closeDrawer();
});

// ========================================================== view route
const navButtons = [...document.querySelectorAll('.nav-item')];
navButtons.forEach((btn) => btn.addEventListener('click', () => {
  navButtons.forEach((b) => b.classList.remove('is-active'));
  btn.classList.add('is-active');
  document.querySelectorAll('.view').forEach((v) => v.classList.remove('is-active'));
  el('view-' + btn.dataset.view).classList.add('is-active');
  closeDrawer();
}));
function switchTo(view) {
  const b = navButtons.find((x) => x.dataset.view === view);
  if (b) b.click();
}

// ============================================================ library
async function loadConfig() {
  try {
    const cfg = await api('/api/config');
    setLibPath(cfg.libraryDir || '—');
  } catch (e) { setLibPath('—'); }
}

function setLibPath(p) {
  el('libPath').textContent = p;
  el('libPath').title = p;
}

async function loadLibrary(refresh) {
  const data = await api('/api/library' + (refresh ? '?refresh=1' : ''));
  state.tracks = (data.tracks || []).map((t) => {
    const artist = deriveArtist(t);
    return artist && artist !== (t.artist || '') ? { ...t, artist } : t;
  });
  renderCategories();
  applyFilterSort();
  updateStats();
  el('libSubtitle').textContent =
    state.tracks.length
      ? `${state.tracks.length} 首曲目 · 总时长 ${fmtDuration(totalSeconds())} · 目录 ${(data.dirs || []).length} 个`
      : '还没有曲目，先提取几首吧';
}

function totalSeconds() { return state.tracks.reduce((a, t) => a + (t.duration || 0), 0); }

function updateStats() {
  el('trackCount').textContent = state.tracks.length;
  el('totalTime').textContent = fmtTime(totalSeconds());
  el('totalSize').textContent = fmtSize(state.tracks.reduce((a, t) => a + (t.size || 0), 0));
}

function sortedTracks(list) {
  const arr = list.slice();
  switch (state.sortKey) {
    case 'title':    return arr.sort((a, b) => cmpText(a.title, b.title));
    case 'artist':   return arr.sort((a, b) => cmpText(a.artist, b.artist) || cmpText(a.title, b.title));
    case 'album':    return arr.sort((a, b) => cmpText(a.album, b.album) || cmpText(a.title, b.title));
    case 'duration': return arr.sort((a, b) => (b.duration || 0) - (a.duration || 0));
    case 'size':     return arr.sort((a, b) => (b.size || 0) - (a.size || 0));
    default:         return arr.sort((a, b) => (b.mtime || 0) - (a.mtime || 0)); // 最近添加
  }
}

function applyFilterSort() {
  const q = el('search').value.trim().toLowerCase();
  state.searching = q.length > 0;
  el('searchClear').hidden = !state.searching;

  let filtered = state.tracks.slice();
  const cat = state.category;
  const pl = cat.kind === 'playlist' ? state.playlists.find((x) => x.id === cat.value) : null;
  if (cat.kind === 'fav')          filtered = filtered.filter((t) => t.favorite);
  else if (cat.kind === 'album')   filtered = filtered.filter((t) => catValue(t, 'album') === cat.value);
  else if (cat.kind === 'artist')  filtered = filtered.filter((t) => catValue(t, 'artist') === cat.value);
  else if (cat.kind === 'playlist') filtered = pl ? filtered.filter((t) => pl.ids.includes(t.id)) : [];

  if (state.searching)
    filtered = filtered.filter((t) => [t.title, t.artist, t.album, t.fileName, t.genre]
        .some((f) => (f || '').toLowerCase().includes(q)));

  state.view = sortedTracks(filtered);
  renderList();
  rebuildOrder();

  el('emptyLib').hidden = state.tracks.length !== 0;
  el('emptySearch').hidden = !(state.searching && state.tracks.length !== 0 && state.view.length === 0);

  const catEmpty = !state.searching && state.tracks.length !== 0 && state.view.length === 0;
  el('emptyCat').hidden = !catEmpty;
  if (catEmpty) {
    const titles = {
      fav: '收藏里还没有曲目',
      album: '这个专辑里没有曲目',
      artist: '这位歌手没有曲目',
      playlist: pl ? `列表「${pl.name}」还没有曲目` : '这个列表还没有曲目',
    };
    const subs = {
      fav: '点击列表右侧的 ♥ 即可把喜欢的歌收进这里，收藏会保存在服务端，所有设备都能看到',
      album: '去「全部曲目」看看其它专辑吧',
      artist: '去「全部曲目」看看其它歌手吧',
      playlist: '在曲目右侧「更多操作 → 添加到列表」把歌曲收进来',
    };
    el('emptyCatTitle').textContent = titles[cat.kind] || '这里还没有曲目';
    el('emptyCatSub').textContent = subs[cat.kind] || '';
  }
  el('playAllBtn').disabled = state.view.length === 0;
}

function coverHtml(t, size) {
  return t && t.hasCover
    ? `<img src="/api/cover?id=${encodeURIComponent(t.id)}" alt="">`
    : '';
}

function renderList() {
  const list = el('trackList');
  list.innerHTML = state.view.map((t, i) => {
    const isCur = t.id === state.currentId;
    const queued = state.queue.includes(t.id);
    return `
    <div class="track ${isCur ? 'is-current' : ''} ${queued ? 'track-queued' : ''}" data-id="${t.id}">
      <div class="track-idx">${
        isCur
          ? `<span class="eq"><i></i><i></i><i></i></span>`
          : `<span>${i + 1}</span>`
      }</div>
      <div style="min-width:0">
        <div class="track-title" title="${esc(t.title)}">${esc(t.title)}</div>
        <div class="track-file" title="${esc(t.fileName)}">${esc(t.fileName)}</div>
      </div>
      <div class="track-artist" title="${esc(t.artist || '')}">${esc(t.artist || '未知歌手')}</div>
      <div class="track-album" title="${esc(t.album || '')}">${esc(t.album || '—')}</div>
      <div class="track-time">${fmtTime(t.duration)}</div>
      <div class="track-size">${fmtSize(t.size)}</div>
      <div class="track-ops">
        <button class="op-btn op-fav ${t.favorite ? 'is-on' : ''}" data-fav="${t.id}"
                title="${t.favorite ? '取消收藏' : '收藏'}">
          <svg class="icon"><use href="#i-${t.favorite ? 'heart' : 'heart-o'}"/></svg>
        </button>
        <button class="op-btn" data-dl="${t.id}" title="下载到本地"><svg class="icon"><use href="#i-download"/></svg></button>
        <button class="op-btn" data-more="${t.id}" title="更多操作"><svg class="icon"><use href="#i-more"/></svg></button>
      </div>
    </div>`;
  }).join('');

  list.querySelectorAll('.track').forEach((row) => {
    row.addEventListener('click', (ev) => {
      if (ev.target.closest('[data-dl]') || ev.target.closest('[data-more]') ||
          ev.target.closest('[data-fav]')) return;
      toggleOrPlay(row.dataset.id);
    });
  });
  list.querySelectorAll('[data-fav]').forEach((b) => b.addEventListener('click', (ev) => {
    ev.stopPropagation();
    toggleFavorite(b.dataset.fav);
  }));
  list.querySelectorAll('[data-dl]').forEach((b) => b.addEventListener('click', (ev) => {
    ev.stopPropagation();
    downloadTrack(b.dataset.dl);
  }));
  list.querySelectorAll('[data-more]').forEach((b) => b.addEventListener('click', (ev) => {
    ev.stopPropagation();
    const r = b.getBoundingClientRect();
    openCtxMenu(b.dataset.more, r.right - 178, r.bottom + 4);
  }));
}

// ========================================================= play queue
function rebuildOrder() {
  const n = state.view.length;
  const idx = [...Array(n).keys()];
  if (state.shuffle) {
    for (let i = n - 1; i > 0; i--) {
      const j = Math.floor(Math.random() * (i + 1));
      [idx[i], idx[j]] = [idx[j], idx[i]];
    }
    if (state.currentId) {
      const cur = state.view.findIndex((t) => t.id === state.currentId);
      const at = idx.indexOf(cur);
      if (at > 0) [idx[0], idx[at]] = [idx[at], idx[0]];
    }
  }
  state.order = idx;
  state.orderPos = state.currentId
    ? idx.indexOf(state.view.findIndex((t) => t.id === state.currentId))
    : -1;
}

const trackById = (id) => state.tracks.find((t) => t.id === id);

function toggleOrPlay(id) {
  if (state.currentId === id) { togglePlay(); return; }
  playTrack(id);
}

function playTrack(id) {
  const idx = state.view.findIndex((t) => t.id === id);
  if (idx < 0) return;
  state.currentId = id;
  if (!state.order.includes(idx)) rebuildOrder();
  state.orderPos = state.order.indexOf(idx);

  // consume an interleaved "play next" entry if present
  const qi = state.queue.indexOf(id);
  if (qi >= 0) state.queue.splice(qi, 1);

  audio.src = '/api/stream?id=' + encodeURIComponent(id);
  audio.play().catch((e) => toast('播放失败：' + e.message));

  updateHero(id);
  renderList();
}

function updateHero(id) {
  const t = trackById(id);
  el('heroTitle').textContent = t ? t.title : '选择一首曲目';
  el('heroArtist').textContent = t ? (t.artist || '未知艺术家') : '点击列表中的任意曲目即可开始播放';
  el('heroEyebrow').textContent = t ? '正在播放' : '未在播放';
  el('heroFormat').textContent = t ? (t.codec || '').toUpperCase() : '—';
  el('heroBitrate').textContent = t && t.bitrate ? t.bitrate + ' kbps' : '—';
  el('heroRate').textContent = t && t.sampleRate ? (t.sampleRate / 1000).toFixed(1) + ' kHz' : '—';
  el('heroAlbum').hidden = !(t && t.album);
  el('heroAlbum').textContent = t && t.album ? t.album : '';

  const disabled = !t;
  el('heroPlay').disabled = disabled;
  el('heroReveal').disabled = disabled;
  el('heroDownload').disabled = disabled;

  const cover = coverHtml(t);
  el('heroCover').innerHTML = cover ? cover : '<span>♪</span>';
  el('npCover').innerHTML = cover ? cover : '<span>♪</span>';

  const bg = el('heroBg');
  if (t && t.hasCover) {
    bg.style.backgroundImage = `url("/api/cover?id=${encodeURIComponent(t.id)}")`;
    bg.classList.add('on');
  } else {
    bg.classList.remove('on');
    bg.style.backgroundImage = '';
  }

  el('npTitle').textContent = t ? t.title : '未在播放';
  el('npArtist').textContent = t ? (t.artist || '未知艺术家') : '—';
  el('totTime').textContent = fmtTime(t ? t.duration : 0);
  updateMediaSession(t);

  // Keep the lyric sidebar in step with whatever is playing.
  if (typeof setLyricSide === 'function') {
    if (lyric.id !== id) { lyric.id = null; lyric.found = false; lyric.lines = []; lyric.offset = 0; }
    setLyricSide();
    if (lyricViewActive()) loadLyricFor(id);
  }
}

function setPlayIcon(playing, loading) {
  icon(el('playIcon'), loading ? 'refresh' : (playing ? 'pause' : 'play'));
  el('playIcon').classList.toggle('spin', !!loading);
  icon(el('heroPlayIcon'), loading ? 'refresh' : (playing ? 'pause' : 'play'));
  el('heroPlayText').textContent = playing ? '暂停' : '播放';
}

function togglePlay() {
  if (!audio.src) {
    if (state.view.length) playTrack(state.view[0].id);
    return;
  }
  audio.paused ? audio.play().catch(() => {}) : audio.pause();
}

function step(dir) {
  const n = state.view.length;
  if (n === 0) return;

  if (dir > 0 && state.queue.length) {
    const nextId = state.queue.shift();
    if (trackById(nextId)) { playTrack(nextId); renderList(); return; }
  }

  let pos = state.orderPos + dir;
  if (pos < 0) pos = state.repeat === 'all' ? n - 1 : 0;
  if (pos >= n) {
    if (state.repeat === 'all') { rebuildOrder(); pos = 0; }
    else { audio.pause(); return; }
  }
  const t = state.view[state.order[pos]];
  if (t) playTrack(t.id);
}

// ====================================================== audio element
audio.addEventListener('loadedmetadata', () => {
  if (isFinite(audio.duration)) el('totTime').textContent = fmtTime(audio.duration);
});
audio.addEventListener('timeupdate', () => {
  if (seeking || !isFinite(audio.duration) || audio.duration === 0) return;
  paintSeek(audio.currentTime / audio.duration);
  el('curTime').textContent = fmtTime(audio.currentTime);
  syncLyric(audio.currentTime, false);
});
audio.addEventListener('progress', () => {
  if (!isFinite(audio.duration) || audio.duration === 0 || !audio.buffered.length) return;
  const end = audio.buffered.end(audio.buffered.length - 1);
  el('seekBuffer').style.width = Math.min(100, (end / audio.duration) * 100) + '%';
});
audio.addEventListener('waiting', () => setPlayIcon(true, true));
audio.addEventListener('playing', () => setPlayIcon(true, false));
audio.addEventListener('play',  () => setPlayIcon(true, false));
audio.addEventListener('pause', () => setPlayIcon(false, false));
audio.addEventListener('ended', () => {
  if (state.repeat === 'one') { audio.currentTime = 0; audio.play().catch(() => {}); }
  else step(1);
});
audio.addEventListener('error', () => {
  if (audio.src) toast('无法播放该曲目（浏览器可能不支持该编码）');
  setPlayIcon(false, false);
});

// =============================================================== seek
let seeking = false;
function paintSeek(ratio) {
  el('seekFill').style.width = Math.min(100, Math.max(0, ratio * 100)) + '%';
}

(function initSeek() {
  const bar = el('seekBar');
  const ratioOf = (ev) => {
    const r = bar.getBoundingClientRect();
    return Math.min(1, Math.max(0, (ev.clientX - r.left) / r.width));
  };
  bar.addEventListener('pointerdown', (ev) => {
    if (!isFinite(audio.duration) || audio.duration === 0) return;
    seeking = true;
    bar.classList.add('dragging');
    bar.setPointerCapture(ev.pointerId);
    const r = ratioOf(ev);
    paintSeek(r);
    el('curTime').textContent = fmtTime(r * audio.duration);
  });
  bar.addEventListener('pointermove', (ev) => {
    if (!seeking) return;
    const r = ratioOf(ev);
    paintSeek(r);
    el('curTime').textContent = fmtTime(r * audio.duration);
  });
  bar.addEventListener('pointerup', (ev) => {
    if (!seeking) return;
    seeking = false;
    bar.classList.remove('dragging');
    const r = ratioOf(ev);
    paintSeek(r);
    if (isFinite(audio.duration) && audio.duration > 0) audio.currentTime = r * audio.duration;
    el('curTime').textContent = fmtTime(r * audio.duration);
  });
  bar.addEventListener('pointercancel', () => { seeking = false; bar.classList.remove('dragging'); });
})();

// ============================================================= volume
let lastVolume = 0.8;
function applyVolume(v, remember) {
  v = Math.min(1, Math.max(0, v));
  audio.volume = v;
  if (v > 0 && remember !== false) lastVolume = v;
  el('volFill').style.width = (v * 100) + '%';
  el('volNum').textContent = Math.round(v * 100);
  const muted = v === 0;
  icon(el('volIcon'), muted ? 'mute' : 'volume');
  el('muteBtn').classList.toggle('is-on', muted);
  try { localStorage.setItem('ae_volume', String(v)); } catch (e) {}
}

(function initVolume() {
  const bar = el('volBar');
  let dragging = false;
  let start = parseFloat(localStorage.getItem('ae_volume') || '0.8');
  if (!isFinite(start)) start = 0.8;
  applyVolume(start);

  const ratioOf = (ev) => {
    const r = bar.getBoundingClientRect();
    return Math.min(1, Math.max(0, (ev.clientX - r.left) / r.width));
  };
  bar.addEventListener('pointerdown', (ev) => {
    dragging = true; bar.classList.add('dragging'); bar.setPointerCapture(ev.pointerId);
    applyVolume(ratioOf(ev));
  });
  bar.addEventListener('pointermove', (ev) => { if (dragging) applyVolume(ratioOf(ev)); });
  const stop = () => { dragging = false; bar.classList.remove('dragging'); };
  bar.addEventListener('pointerup', stop);
  bar.addEventListener('pointercancel', stop);

  el('muteBtn').addEventListener('click', () => {
    applyVolume(audio.volume > 0 ? 0 : (lastVolume || 0.8));
  });
})();

// ========================================================= transport
el('playBtn').addEventListener('click', togglePlay);
el('prevBtn').addEventListener('click', () => {
  if (audio.currentTime > 3) { audio.currentTime = 0; return; }
  step(-1);
});
el('nextBtn').addEventListener('click', () => step(1));
el('shuffleBtn').addEventListener('click', () => {
  state.shuffle = !state.shuffle;
  el('shuffleBtn').classList.toggle('is-on', state.shuffle);
  localStorage.setItem('ae_shuffle', state.shuffle ? '1' : '0');
  rebuildOrder();
  toast(state.shuffle ? '随机播放：开' : '随机播放：关');
});

function applyRepeat(mode) {
  state.repeat = mode;
  el('repeatBtn').classList.toggle('is-on', mode !== 'off');
  icon(el('repeatIcon'), mode === 'one' ? 'repeat-one' : 'repeat');
  el('repeatBtn').title = { off: '不循环', all: '列表循环', one: '单曲循环' }[mode];
  localStorage.setItem('ae_repeat', mode);
}
el('repeatBtn').addEventListener('click', () => {
  const modes = ['off', 'all', 'one'];
  const mode = modes[(modes.indexOf(state.repeat) + 1) % modes.length];
  applyRepeat(mode);
  toast({ off: '不循环', all: '列表循环', one: '单曲循环' }[mode]);
});

document.addEventListener('keydown', (ev) => {
  const tag = (ev.target.tagName || '').toLowerCase();
  const typing = tag === 'input' || tag === 'select' || tag === 'textarea' || ev.target.isContentEditable;
  if (ev.code === 'Escape') { closeCtxMenu(); return; }
  if (!el('dirModal').hidden && ev.code === 'Escape') { closeDirModal(); return; }
  if (typing) {
    if (ev.code === 'Escape') ev.target.blur();
    return;
  }
  switch (ev.code) {
    case 'Space':   ev.preventDefault(); togglePlay(); break;
    case 'ArrowRight': ev.preventDefault(); step(1); break;
    case 'ArrowLeft':  ev.preventDefault(); step(-1); break;
    case 'ArrowUp':    ev.preventDefault(); applyVolume(audio.volume + 0.05); break;
    case 'ArrowDown':  ev.preventDefault(); applyVolume(audio.volume - 0.05); break;
    case 'Slash':   ev.preventDefault(); el('search').focus(); break;
    case 'KeyL':    ev.preventDefault(); toggleLyricView(); break;
    case 'KeyM':    el('muteBtn').click(); break;
    case 'KeyS':    el('shuffleBtn').click(); break;
    case 'KeyR':    el('repeatBtn').click(); break;
  }
});

// ====================================================== media session
function updateMediaSession(t) {
  if (!('mediaSession' in navigator)) return;
  try {
    navigator.mediaSession.metadata = t ? new MediaMetadata({
      title: t.title,
      artist: t.artist || '未知艺术家',
      album: t.album || '',
      artwork: t.hasCover ? [{ src: `/api/cover?id=${encodeURIComponent(t.id)}`, sizes: '512x512' }] : [],
    }) : null;
    navigator.mediaSession.setActionHandler('play', togglePlay);
    navigator.mediaSession.setActionHandler('pause', togglePlay);
    navigator.mediaSession.setActionHandler('previoustrack', () => step(-1));
    navigator.mediaSession.setActionHandler('nexttrack', () => step(1));
  } catch (e) { /* unsupported */ }
}

// =========================================================== actions
function downloadTrack(id) {
  const t = trackById(id);
  if (!t) return;
  const a = document.createElement('a');
  a.href = '/api/download?id=' + encodeURIComponent(id);
  a.download = t.fileName;
  document.body.appendChild(a);
  a.click();
  a.remove();
  toast('已开始下载：' + t.fileName);
}

async function revealTrack(id) {
  const t = trackById(id);
  if (!t) return;
  try {
    await api('/api/reveal', {
      method: 'POST', headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ id }),
    });
  } catch (e) { toast('打开失败：' + e.message); }
}

function playNext(id) {
  if (!trackById(id)) return;
  const i = state.queue.indexOf(id);
  if (i >= 0) state.queue.splice(i, 1);
  state.queue.push(id);
  renderList();
  toast('已加入「下一首播放」');
}

// ------------------------------------------------------ context menu
const ctxMenu = el('ctxMenu');
function openCtxMenu(id, x, y) {
  state.ctxTarget = id;
  ctxMenu.hidden = false;
  const r = ctxMenu.getBoundingClientRect();
  ctxMenu.style.left = Math.max(8, Math.min(x, window.innerWidth - r.width - 8)) + 'px';
  ctxMenu.style.top = Math.max(8, Math.min(y, window.innerHeight - r.height - 8)) + 'px';
}
function closeCtxMenu() { ctxMenu.hidden = true; state.ctxTarget = null; }

ctxMenu.querySelectorAll('button').forEach((b) => b.addEventListener('click', () => {
  const id = state.ctxTarget;
  const act = b.dataset.act;
  closeCtxMenu();
  if (!id) return;
  if (act === 'play') playTrack(id);
  else if (act === 'queue') playNext(id);
  else if (act === 'download') downloadTrack(id);
  else if (act === 'reveal') revealTrack(id);
  else if (act === 'fav') toggleFavorite(id);
  else if (act === 'addto') openAddToList(id);
  else if (act === 'lyric') {
    if (state.currentId !== id) playTrack(id);
    switchTo('lyric');
    setTimeout(() => fetchLyricAutomatically(id), 60);
  }
  else if (act === 'delete') openDeleteDialog(id);
}));

// ------------------------------------------------------ delete a track
let delTarget = null;

function openDeleteDialog(id) {
  const t = trackById(id);
  if (!t) return;
  delTarget = id;
  el('delTitle').textContent = t.title || t.fileName;
  el('delSub').textContent = [t.artist, t.album].filter(Boolean).join(' · ') || '未知艺术家';
  el('delPath').textContent = t.fileName + (t.size ? ' · ' + fmtSize(t.size) : '');
  const hasLyric = !!t.hasLyric;
  el('delLrcRow').hidden = !hasLyric;
  if (hasLyric) el('delLrcName').textContent = t.fileName.replace(/\.[^.]+$/, '') + '.lrc';
  el('delConfirm').disabled = false;
  el('delConfirm').textContent = '删除并同时删除歌词';
  el('delModal').hidden = false;
}

function closeDeleteDialog() { el('delModal').hidden = true; delTarget = null; }

async function confirmDelete() {
  const id = delTarget;
  if (!id) return;
  const t = trackById(id);
  const btn = el('delConfirm');
  btn.disabled = true;
  btn.textContent = '删除中…';
  try {
    // Release the stream first: on Windows the server cannot unlink a file
    // that this page is still reading.
    if (state.currentId === id) {
      audio.pause();
      audio.removeAttribute('src');
      audio.load();
      await new Promise((r) => setTimeout(r, 260));
    }
    const r = await api('/api/track?id=' + encodeURIComponent(id), { method: 'DELETE' });
    const name = (r && r.title) || (t && t.title) || '曲目';
    const withLyric = (r && r.lyricDeleted) || (t && t.hasLyric);
    closeDeleteDialog();
    await refreshAfterDelete(id, !!withLyric);
    toast('已删除：' + name + (withLyric ? '（含歌词）' : ''));
  } catch (e) {
    toast('删除失败：' + e.message);
  } finally {
    btn.disabled = false;
    btn.textContent = '删除并同时删除歌词';
  }
}

/* Every list has to follow a deletion: play queue, favourites, playlists,
 * album/artist groups and the lyric pane. Reloading from the server keeps
 * all of them authoritative instead of patching each copy by hand. */
async function refreshAfterDelete(id, hadLyric) {
  state.queue = state.queue.filter((q) => q !== id);
  if (state.currentId === id) {
    audio.pause();
    audio.removeAttribute('src');
    audio.load();
    state.currentId = null;
    Object.assign(lyric, {
      id: null, found: false, lines: [], synced: false, localPath: '',
      source: '', offset: 0, active: -1, busy: false,
    });
    lyric.tried = new Set();
    setPlayIcon(false, false);
    updateHero(null);
    renderLyricLines();
    syncLyricControls();
  } else if (hadLyric && lyric.id === id) {
    lyric.id = null;
    lyric.found = false;
    lyric.lines = [];
    lyric.localPath = '';
    lyric.source = '';
    setLyricSide();
    renderLyricLines();
  }
  try {
    const p = await api('/api/playlists');
    state.playlists = p.playlists || [];
  } catch (e) { /* keep whatever we had */ }
  await loadLibrary(true);   // re-renders categories, rows, queue and stats
}
document.addEventListener('pointerdown', (ev) => {
  if (!ctxMenu.hidden && !ctxMenu.contains(ev.target)) closeCtxMenu();
});
window.addEventListener('blur', closeCtxMenu);

// ======================================================== top actions
el('search').addEventListener('input', applyFilterSort);
el('searchClear').addEventListener('click', () => {
  el('search').value = '';
  applyFilterSort();
  el('search').focus();
});
el('clearSearch').addEventListener('click', () => el('searchClear').click());
el('sortSelect').addEventListener('change', () => {
  state.sortKey = el('sortSelect').value;
  localStorage.setItem('ae_sort', state.sortKey);
  applyFilterSort();
});
el('refreshBtn').addEventListener('click', async () => {
  const btn = el('refreshBtn');
  btn.querySelector('.icon').classList.add('spin');
  try { await loadLibrary(true); toast('曲库已刷新'); }
  catch (e) { toast('刷新失败：' + e.message); }
  finally { btn.querySelector('.icon').classList.remove('spin'); }
});
el('playAllBtn').addEventListener('click', () => {
  if (!state.view.length) { toast('曲库是空的'); return; }
  if (state.currentId && state.view.some((t) => t.id === state.currentId)) togglePlay();
  else playTrack(state.view[0].id);
});
el('goExtract').addEventListener('click', () => switchTo('extract'));
el('goBrowse').addEventListener('click', () => openDirModal());
el('browseDir').addEventListener('click', () => openDirModal());
el('revealDir').addEventListener('click', async () => {
  const dir = el('libPath').textContent;
  if (!dir || dir === '—') return;
  try {
    await api('/api/reveal', {
      method: 'POST', headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ path: dir }),
    });
  } catch (e) { toast('打开失败：' + e.message); }
});
el('themeBtn').addEventListener('click', () => applyTheme(state.theme === 'dark' ? 'light' : 'dark'));

el('heroPlay').addEventListener('click', togglePlay);
el('heroReveal').addEventListener('click', () => state.currentId && revealTrack(state.currentId));
el('heroDownload').addEventListener('click', () => state.currentId && downloadTrack(state.currentId));

// ==================================================== directory modal
async function browseDir(path) {
  const list = el('dirList');
  list.innerHTML = '<div class="modal-loading">载入中…</div>';
  try {
    const data = await api('/api/browse?path=' + encodeURIComponent(path));
    state.browsePath = data.path || '';
    const cur = el('libPath').textContent;

    let html = '';
    if (!state.browsePath && (data.drives || []).length) {
      html += '<div class="dir-drives">' + data.drives.map((d) =>
        `<button class="dir-drive" data-path="${esc(d.path)}">
           <svg class="icon"><use href="#i-window"/></svg>${esc(d.name)}
         </button>`).join('') + '</div>';
    }
    if (!data.dirs.length) {
      html += `<div class="modal-loading">${state.browsePath ? '此目录下没有子文件夹' : '选择上方的磁盘'}</div>`;
    } else {
      html += data.dirs.map((d) => `
        <button class="dir-item ${d.path === cur ? 'is-sel' : ''}" data-path="${esc(d.path)}">
          <svg class="icon"><use href="#i-folder"/></svg>
          <span class="dir-name">${esc(d.name)}</span>
          <em class="dir-enter">›</em>
        </button>`).join('');
    }
    list.innerHTML = html;
    el('dirPathInput').value = state.browsePath;
    el('dirCurrent').textContent = state.browsePath || '（未选择）';
    el('dirSelect').disabled = !state.browsePath;
    el('dirUp').disabled = !data.parent;

    list.querySelectorAll('[data-path]').forEach((b) => {
      b.addEventListener('click', () => browseDir(b.dataset.path));
    });
    list._parent = data.parent || '';
  } catch (e) {
    list.innerHTML = `<div class="modal-loading">读取失败：${esc(e.message)}</div>`;
  }
}

function openDirModal() {
  el('dirModal').hidden = false;
  browseDir(el('libPath').textContent !== '—' ? el('libPath').textContent : '');
}
function closeDirModal() { el('dirModal').hidden = true; }

el('dirClose').addEventListener('click', closeDirModal);
el('dirCancel').addEventListener('click', closeDirModal);
el('dirModal').addEventListener('pointerdown', (ev) => {
  if (ev.target === el('dirModal')) closeDirModal();
});
el('dirUp').addEventListener('click', () => {
  const p = el('dirList')._parent;
  if (p) browseDir(p);
});
el('dirGo').addEventListener('click', () => browseDir(el('dirPathInput').value.trim()));
el('dirPathInput').addEventListener('keydown', (ev) => {
  if (ev.key === 'Enter') { ev.preventDefault(); browseDir(el('dirPathInput').value.trim()); }
});
el('dirSelect').addEventListener('click', async () => {
  const dir = state.browsePath;
  if (!dir) return;
  const btn = el('dirSelect');
  btn.disabled = true;
  try {
    await api('/api/config', {
      method: 'POST', headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ libraryDir: dir }),
    });
    setLibPath(dir);
    await loadLibrary(true);
    closeDirModal();
    toast('曲库目录已更新');
  } catch (e) {
    toast('设置失败：' + e.message);
  } finally {
    btn.disabled = false;
  }
});

// ============================================================ lyrics
// Lyric timeline state. `offset` is the user's current nudge in seconds
// (positive = lyric appears later); committing it rewrites the .lrc so the
// adjustment survives a reload.
const lyric = {
  id: null, lines: [], synced: false, source: '', localPath: '',
  found: false, offset: 0, active: -1, busy: false, tried: new Set(), hits: [],
};

const LYRIC_SOURCE_LABEL = {
  local: '本地 .lrc', embedded: '文件内嵌', lrclib: 'LRCLIB', netease: '网易云音乐',
};

const lyricViewActive = () => el('view-lyric').classList.contains('is-active');
let lyricScrollLockUntil = 0;

// Single source of truth for lyric control enablement. Called by setLyricSide
// (which runs on every state change) rather than only from the fetch path —
// otherwise reading a lyric straight off disk would leave the buttons stuck
// in whatever state boot initialised them to.
function syncLyricControls() {
  const locked = lyric.busy || !state.currentId;
  const f = el('lyricFetch');
  const p = el('lyricPick');
  if (f) f.disabled = locked;
  if (p) p.disabled = locked;
}

function lyricBusy(busy) {
  lyric.busy = busy;
  syncLyricControls();
}

// Reveal helper for every branch of the empty / error states.
function lyricEmpty({ icon = 'lyric', title, sub, actions = '' }) {
  el('lyricInner').innerHTML = `
    <div class="lyric-empty">
      <div class="empty-ico"><svg class="icon"><use href="#i-${icon}"/></svg></div>
      <div class="empty-title">${esc(title)}</div>
      ${sub ? `<div class="empty-sub">${esc(sub)}</div>` : ''}
      <div class="empty-actions">${actions}</div>
    </div>`;
}

function setLyricSide() {
  const t = trackById(state.currentId);
  const cover = coverHtml(t);
  el('lyricCover').innerHTML = cover ? cover : '<span>♪</span>';
  el('lyricSideTitle').textContent = t ? t.title : '未在播放';
  el('lyricSideArtist').textContent = t ? (t.artist || '未知艺术家') : '—';
  el('lyricSource').textContent =
    lyric.found && lyric.id === state.currentId
      ? (LYRIC_SOURCE_LABEL[lyric.source] || lyric.source || '已载入')
      : (t ? '暂无歌词' : '—');
  el('lyricMode').hidden = !(lyric.found && lyric.synced);
  el('lyricMode').textContent = '逐行高亮';
  el('lyricOffset').textContent = (lyric.offset >= 0 ? '' : '−') +
    Math.abs(lyric.offset).toFixed(1) + 's';
  el('lyricRemove').hidden = !(lyric.found && lyric.source === 'local');
  el('lyricOpenFile').hidden = !(lyric.found && lyric.localPath);
  syncLyricControls();
  updateLyricDot();
}

function updateLyricDot() {
  const dot = el('lyricDot');
  if (!dot) return;
  const has = lyric.found && lyric.id === state.currentId;
  dot.hidden = has || !state.currentId;
}

function renderLyricLines() {
  if (!state.currentId) {
    lyricEmpty({
      icon: 'play', title: '还没有在播放',
      sub: '回到音乐库点一首歌，再回到这里查看歌词。',
      actions: '<button class="btn btn-primary" data-lyric="toLibrary">去音乐库</button>',
    });
    return;
  }
  if (lyric.busy && !lyric.found) {
    el('lyricInner').innerHTML =
      '<div class="lyric-empty"><div class="empty-ico spin"><svg class="icon">' +
      '<use href="#i-refresh"/></svg></div><div class="empty-title">正在联网检索歌词…</div>' +
      '<div class="empty-sub">同时查询 LRCLIB 与网易云音乐</div></div>';
    return;
  }
  if (!lyric.found || lyric.id !== state.currentId) {
    lyricEmpty({
      icon: 'lyric', title: '这首还没有歌词',
      sub: '可以联网自动抓取，也可以自己挑一个版本。',
      actions:
        '<button class="btn btn-primary" data-lyric="fetch">' +
        '<svg class="icon"><use href="#i-cloud"/></svg>自动获取歌词</button>' +
        '<button class="btn btn-ghost" data-lyric="pick">' +
        '<svg class="icon"><use href="#i-search"/></svg>手动选择版本</button>',
    });
    return;
  }
  if (!lyric.synced) {
    el('lyricInner').innerHTML =
      '<div class="lyric-note"><svg class="icon"><use href="#i-info"/></svg>' +
      '这份歌词没有时间轴，无法逐行跟随，已完整显示如下。</div>' +
      lyric.lines.map((l) => `<p class="lyric-line is-static">${esc(l.text)}</p>`).join('');
    return;
  }
  el('lyricInner').innerHTML = lyric.lines.map((l, i) =>
    `<p class="lyric-line" data-i="${i}" data-t="${(+l.t).toFixed(2)}">${esc(l.text)}</p>`).join('');
  el('lyricInner').querySelectorAll('.lyric-line').forEach((p) => {
    p.addEventListener('click', () => {
      const t = parseFloat(p.dataset.t);
      if (!isFinite(t) || !isFinite(audio.duration)) return;
      audio.currentTime = Math.min(audio.duration - 0.05, Math.max(0, t + lyric.offset));
      if (audio.paused) audio.play().catch(() => {});
    });
  });
  syncLyric(audio.currentTime, true);
}

// Highlights the line that matches `now` and keeps it centred. Called on every
// timeupdate; cheap because it early-returns when the index has not moved.
function syncLyric(now, force) {
  if (!lyric.found || !lyric.synced) return;
  if (lyric.id !== state.currentId) return;
  const adjusted = now - lyric.offset;
  let idx = -1;
  for (let i = 0; i < lyric.lines.length; i++) {
    if (lyric.lines[i].t <= adjusted) idx = i; else break;
  }
  if (idx === lyric.active && !force) return;
  lyric.active = idx;

  const lines = el('lyricInner').querySelectorAll('.lyric-line');
  lines.forEach((p, i) => {
    p.classList.toggle('is-active', i === idx);
    p.classList.toggle('is-past', i < idx);
  });

  const active = lines[idx];
  if (!active) return;
  if (!lyricViewActive()) return;                       // no scrolling on hidden views
  if (Date.now() < lyricScrollLockUntil) return;        // user is scrolling themselves
  const body = el('lyricBody');
  const target = active.offsetTop - body.clientHeight / 2 + active.offsetHeight / 2;
  body.scrollTo({ top: Math.max(0, target), behavior: 'smooth' });
}

async function loadLyricFor(id) {
  if (!id) { lyric.found = false; lyric.id = null; return; }
  try {
    const data = await api('/api/lyric?id=' + encodeURIComponent(id));
    const l = data.lyric || {};
    lyric.id = id;
    lyric.found = !!l.found;
    lyric.lines = l.lines || [];
    lyric.synced = !!l.synced;
    lyric.source = l.source || '';
    lyric.localPath = l.localPath || '';
    lyric.offset = 0;
    lyric.active = -1;
  } catch (e) {
    lyric.id = id; lyric.found = false; lyric.lines = []; lyric.offset = 0;
  }
  lyricScrollLockUntil = 0;
  setLyricSide();
  if (lyricViewActive()) renderLyricLines();

  // Ripple the auto-fetch once per track, but only while the user is actually
  // looking at this screen — never as background traffic during playback.
  if (!lyric.found && lyricViewActive() && !lyric.tried.has(id)) {
    lyric.tried.add(id);
    fetchLyricAutomatically(id);
  }
}

async function fetchLyricAutomatically(id) {
  lyric.tried.add(id);
  lyricBusy(true);
  renderLyricLines();
  setLyricSide();
  try {
    const data = await api('/api/lyric/auto', {
      method: 'POST', headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ id }),
    });
    applyLyricPayload(data.lyric, id);
    const src = LYRIC_SOURCE_LABEL[data.hit && data.hit.provider] || '网络';
    toast('已保存歌词：' + ((data.hit && data.hit.title) || '') + ' · 来源 ' + src);
    return true;
  } catch (e) {
    setLyricSide();
    lyricEmpty({
      icon: 'lyric', title: '没能自动匹配到歌词',
      sub: e.message || '可以尝试手动指定歌曲名 / 歌手再搜一次。',
      actions:
        '<button class="btn btn-primary" data-lyric="pick">' +
        '<svg class="icon"><use href="#i-search"/></svg>手动选择版本</button>' +
        '<button class="btn btn-ghost" data-lyric="retry">重试自动获取</button>',
    });
    return false;
  } finally {
    // Re-render after the busy flag flips: if the fetch succeeded the payload
    // was painted while `busy` was still true, so this is what actually
    // replaces the spinner with the lyric.
    lyricBusy(false);
    setLyricSide();
    renderLyricLines();
  }
}

function applyLyricPayload(l, id) {
  lyric.id = id || state.currentId;
  lyric.found = !!l.found;
  lyric.lines = l.lines || [];
  lyric.synced = !!l.synced;
  lyric.source = l.source || '';
  lyric.localPath = l.localPath || '';
  lyric.offset = 0;
  lyric.active = -1;
  lyricScrollLockUntil = 0;
  setLyricSide();
  renderLyricLines();
}

// ------------------------------------------------------- candidate list
async function openLyricPicker(prefill) {
  if (!state.currentId) { toast('先在音乐库里选一首歌'); return; }
  const t = trackById(state.currentId);
  el('lyricModal').hidden = false;
  el('lyricQueryTitle').value = (prefill && prefill.title) || (t ? t.title : '');
  el('lyricQueryArtist').value = (prefill && prefill.artist) || (t ? t.artist || '' : '');
  await runLyricSearch();
}

async function runLyricSearch() {
  const box = el('lyricResults');
  box.innerHTML = '<div class="modal-loading">正在检索…</div>';
  el('lyricResultsInfo').textContent = '检索中…';
  try {
    const data = await api('/api/lyric/search', {
      method: 'POST', headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({
        id: state.currentId,
        title: el('lyricQueryTitle').value.trim(),
        artist: el('lyricQueryArtist').value.trim(),
      }),
    });
    lyric.hits = data.hits || [];
    if (!lyric.hits.length) {
      box.innerHTML = `<div class="lyric-note"><svg class="icon"><use href="#i-info"/></svg>` +
        `没有找到匹配的歌词${data.error ? '：' + esc(data.error) : ''}。` +
        `换个歌曲名 / 歌手再试试。</div>`;
      el('lyricResultsInfo').textContent = '0 个结果';
      return;
    }
    box.innerHTML = lyric.hits.map((h, i) => `
      <button class="lyric-hit ${i === 0 ? 'is-best' : ''}" data-hit="${i}">
        <div class="lyric-hit-top">
          <span class="lyric-hit-title">${esc(h.title)}</span>
          <span class="lyric-hit-src">${esc(LYRIC_SOURCE_LABEL[h.provider] || h.provider)}</span>
        </div>
        <div class="lyric-hit-sub">
          ${esc(h.artist || '未知艺术家')}${h.album ? ' · ' + esc(h.album) : ''}
          ${h.duration ? ' · ' + fmtTime(h.duration) : ''}
          <span class="lyric-hit-flag">${h.synced ? '有时间轴' : '纯文本'}</span>
        </div>
        <div class="lyric-hit-preview">${esc(h.preview || '')}</div>
        <div class="lyric-hit-score">匹配度 ${Math.min(100, Math.round(h.score))}</div>
      </button>`).join('');
    el('lyricResultsInfo').textContent =
      `共 ${lyric.hits.length} 个结果 · 按匹配度排序（第一位已推荐）`;
    box.querySelectorAll('[data-hit]').forEach((b) => b.addEventListener('click', () => {
      saveLyricHit(parseInt(b.dataset.hit, 10));
    }));
  } catch (e) {
    box.innerHTML = `<div class="lyric-note">检索失败：${esc(e.message)}</div>`;
    el('lyricResultsInfo').textContent = '检索失败';
  }
}

async function saveLyricHit(i) {
  const h = lyric.hits[i];
  if (!h) return;
  try {
    const data = await api('/api/lyric/save', {
      method: 'POST', headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ id: state.currentId, provider: h.provider, remoteId: h.remoteId }),
    });
    applyLyricPayload(data.lyric);
    el('lyricModal').hidden = true;
    toast('歌词已保存到音乐目录');
  } catch (e) {
    toast('保存失败：' + e.message);
  }
}

// ------------------------------------------------------------- offsets
// Rewrites the .lrc with the shift baked into every timestamp so the change
// persists; the in-memory offset is then reset to zero.
async function commitOffset(delta) {
  if (!lyric.found || !lyric.synced || lyric.id !== state.currentId) {
    toast('先加载一份带时间轴的歌词');
    return;
  }
  lyric.offset += delta;
  el('lyricOffset').textContent = (lyric.offset >= 0 ? '' : '−') +
    Math.abs(lyric.offset).toFixed(1) + 's';
  syncLyric(audio.currentTime, true);

  const lrcText = [`[ti:${(trackById(lyric.id) || {}).title || ''}]`,
    `[ar:${(trackById(lyric.id) || {}).artist || ''}]`]
    .concat(lyric.lines.map((l) => {
      const t = Math.max(0, l.t + lyric.offset);
      const m = Math.floor(t / 60), s = Math.floor(t % 60), cs = Math.round((t % 1) * 100);
      return `[${String(m).padStart(2, '0')}:${String(s).padStart(2, '0')}.` +
             `${String(Math.min(99, cs)).padStart(2, '0')}]${l.text}`;
    })).join('\n');
  try {
    await api('/api/lyric/save', {
      method: 'POST', headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ id: lyric.id, lrc: lrcText }),
    });
    lyric.lines = lyric.lines.map((l) => ({ t: Math.max(0, l.t + lyric.offset), text: l.text }));
    lyric.offset = 0;
    lyric.active = -1;
    setLyricSide();
    renderLyricLines();
  } catch (e) {
    toast('保存偏移失败：' + e.message);
  }
}

// =========================================================== extract
const STATE_LABEL = { queued: '排队中', running: '进行中', done: '已完成', failed: '失败' };
const STAGE_LABEL = { queued: '排队中', starting: '准备中', downloading: '下载媒体', transcoding: '转码音频', done: '完成', error: '出错' };

el('extractForm').addEventListener('submit', async (ev) => {
  ev.preventDefault();
  const url = el('urlInput').value.trim();
  if (!url) { toast('请填写视频链接'); el('urlInput').focus(); return; }
  const btn = el('extractBtn');
  const label = btn.querySelector('span');
  btn.disabled = true;
  const original = label.textContent;
  label.textContent = '提交中…';
  try {
    await api('/api/extract', {
      method: 'POST', headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({
        url,
        output: el('outInput').value.trim(),
        format: el('fmtSelect').value,
        bitrate: parseInt(el('brSelect').value, 10),
      }),
    });
    el('urlInput').value = '';
    el('outInput').value = '';
    toast('任务已加入队列');
    await pollTasks();
  } catch (e) {
    toast('提交失败：' + e.message);
  } finally {
    btn.disabled = false;
    label.textContent = original;
  }
});

el('clearTasks').addEventListener('click', async () => {
  try { await api('/api/tasks/clear', { method: 'POST' }); await pollTasks(); }
  catch (e) { toast('清除失败：' + e.message); }
});

function renderTasks() {
  const tasks = state.tasks;
  el('emptyTasks').hidden = tasks.length !== 0;
  const badge = el('taskBadge');
  const running = tasks.filter((t) => t.status === 'running' || t.status === 'queued').length;
  badge.hidden = running === 0;
  badge.textContent = running;

  el('taskList').innerHTML = tasks.slice().reverse().map((t) => {
    const pct = Math.round((t.progress || 0) * 100);
    const name = t.output || t.title || t.url;
    const busy = t.status === 'running' || t.status === 'queued';
    return `
      <div class="task">
        <div class="task-top">
          <div class="task-icon">
            <svg class="icon ${busy ? 'spin' : ''}"><use href="#i-${busy ? 'refresh' : (t.status === 'done' ? 'check' : (t.status === 'failed' ? 'close' : 'info'))}"/></svg>
          </div>
          <div class="task-main">
            <div class="task-title" title="${esc(name)}">${esc(name)}</div>
            <div class="task-url" title="${esc(t.url)}">${esc(t.url)}</div>
          </div>
          <div class="task-state state-${t.status}">${STATE_LABEL[t.status] || t.status}</div>
        </div>
        ${busy
          ? `<div class="task-bar"><i style="width:${Math.max(3, pct)}%"></i></div>
             <div class="task-foot"><span>${esc(STAGE_LABEL[t.stage] || t.stage)}</span><span class="task-pct">${pct}%</span></div>`
          : ''}
        ${t.status === 'failed' ? `<div class="task-err">${esc(t.error || '未知错误')}</div>` : ''}
        ${t.status === 'done' ? `<div class="task-foot"><span>${esc(t.format ? t.format.toUpperCase() : '')} · ${t.bitrate} kbps</span><span class="task-pct">${esc(t.output || '')}</span></div>` : ''}
      </div>`;
  }).join('');
}

let lastDoneCount = -1;
async function pollTasks() {
  try {
    const data = await api('/api/tasks');
    state.tasks = data.tasks || [];
    renderTasks();
    const done = state.tasks.filter((t) => t.status === 'done').length;
    const active = state.tasks.some((t) => t.status === 'running' || t.status === 'queued');
    if (done > lastDoneCount && lastDoneCount >= 0) {
      await loadLibrary(true);
      const t = state.tasks.find((x) => x.status === 'done');
      toast('已加入曲库：' + (t ? (t.output || t.title) : ''));
      switchTo('library');
    }
    lastDoneCount = done;
    if (active) setTimeout(pollTasks, 1200);
    else if (state.tasks.length) setTimeout(pollTasks, 6000);
  } catch (e) { /* backend temporarily unreachable */ }
}

// =================================================== lyric wiring
function onLyricViewEnter() {
  if (state.currentId && lyric.id !== state.currentId) loadLyricFor(state.currentId);
  else renderLyricLines();
}

function toggleLyricView() {
  if (lyricViewActive()) switchTo('library');
  else { switchTo('lyric'); onLyricViewEnter(); }
}

navButtons.forEach((btn) => btn.addEventListener('click', () => {
  if (btn.dataset.view === 'lyric') onLyricViewEnter();
}));

el('lyricInner').addEventListener('click', (ev) => {
  const b = ev.target.closest('[data-lyric]');
  if (!b) return;
  const act = b.dataset.lyric;
  if (act === 'toLibrary') switchTo('library');
  else if (act === 'fetch') fetchLyricAutomatically(state.currentId);
  else if (act === 'pick') openLyricPicker();
  else if (act === 'retry') {
    lyric.tried.delete(state.currentId);
    fetchLyricAutomatically(state.currentId);
  }
});

el('lyricFetch').addEventListener('click', () => {
  if (!state.currentId) { toast('先在音乐库里选一首歌'); return; }
  fetchLyricAutomatically(state.currentId);
});
el('lyricPick').addEventListener('click', () => openLyricPicker());
el('lyricBtn').addEventListener('click', toggleLyricView);
el('heroLyric').addEventListener('click', () => { switchTo('lyric'); onLyricViewEnter(); });

el('lyricOffsetUp').addEventListener('click', () => commitOffset(+0.5));
el('lyricOffsetDown').addEventListener('click', () => commitOffset(-0.5));

el('lyricRemove').addEventListener('click', async () => {
  if (!lyric.id) return;
  try {
    const r = await api('/api/lyric?id=' + encodeURIComponent(lyric.id), { method: 'DELETE' });
    lyric.found = false; lyric.lines = []; lyric.offset = 0; lyric.source = ''; lyric.localPath = '';
    setLyricSide(); renderLyricLines();
    toast(r.removed ? '已删除本地歌词文件' : '本来就没有歌词文件');
  } catch (e) { toast('删除失败：' + e.message); }
});

el('lyricOpenFile').addEventListener('click', async () => {
  if (!lyric.localPath) return;
  try {
    await api('/api/reveal', {
      method: 'POST', headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ path: lyric.localPath }),
    });
  } catch (e) { toast('打开失败：' + e.message); }
});

// Pause auto-scroll briefly while the user scrolls the lyric themselves.
['wheel', 'pointerdown'].forEach((evName) => {
  el('lyricBody').addEventListener(evName, () => {
    lyricScrollLockUntil = Date.now() + 4000;
  }, { passive: true });
});

function closeLyricModal() { el('lyricModal').hidden = true; }
el('lyricModalClose').addEventListener('click', closeLyricModal);
el('lyricModalCancel').addEventListener('click', closeLyricModal);
el('lyricModal').addEventListener('pointerdown', (ev) => {
  if (ev.target === el('lyricModal')) closeLyricModal();
});
el('lyricQueryGo').addEventListener('click', runLyricSearch);
[el('lyricQueryTitle'), el('lyricQueryArtist')].forEach((i) => {
  i.addEventListener('keydown', (ev) => {
    if (ev.key === 'Enter') { ev.preventDefault(); runLyricSearch(); }
  });
});

// =============================================================== boot
(function boot() {
  applyTheme(state.theme);
  applyRepeat(state.repeat);
  el('shuffleBtn').classList.toggle('is-on', state.shuffle);
  el('sortSelect').value = state.sortKey;
  setPlayIcon(false, false);
  setLyricSide();
  renderLyricLines();
  lyricBusy(false);

  state.category = loadCategoryPref();
  state.playlists = [];
  renderCategories();
  loadConfig();
  loadLibrary(false).catch((e) => toast('曲库载入失败：' + e.message));
  loadPlaylists();
  pollTasks();
  setInterval(() => { if (!document.hidden) pollTasks(); }, 12000);
})();
