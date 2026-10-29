/* ===================================================================
 * Shared helpers for the "Audio Extractor → 曲库" browser extension.
 *
 * Loaded twice: once from popup.html and once from background.js via
 * importScripts(). Therefore it must stay free of any DOM assumptions
 * and only exposes a single global object named AE.
 * =================================================================== */
(function (scope) {
  'use strict';

  const DEFAULTS = {
    serverUrl: 'http://127.0.0.1:8080',
    format: 'mp3',
    bitrate: 320,
    lastTaskId: 0,
  };

  function normalizeBase(raw) {
    const s = String(raw || '').trim().replace(/\/+$/, '');
    if (!s) return DEFAULTS.serverUrl;
    if (!/^https?:\/\//i.test(s)) return 'http://' + s;
    return s;
  }

  const AE = {
    DEFAULTS,

    /** Read the persisted settings, falling back to the built-in defaults. */
    async settings() {
      try {
        const got = await chrome.storage.local.get(['aeSettings']);
        return Object.assign({}, DEFAULTS, (got && got.aeSettings) || {});
      } catch (e) {
        return Object.assign({}, DEFAULTS);
      }
    },

    async save(patch) {
      const cur = await AE.settings();
      const next = Object.assign({}, cur, patch || {}, { serverUrl: normalizeBase((patch && patch.serverUrl) || cur.serverUrl) });
      try {
        await chrome.storage.local.set({ aeSettings: next });
      } catch (e) { /* ignore */ }
      return next;
    },

    async base() {
      const s = await AE.settings();
      return normalizeBase(s.serverUrl);
    },

    /** Is the local player reachable? */
    async health() {
      const base = await AE.base();
      const ctl = new AbortController();
      const timer = setTimeout(() => ctl.abort(), 2500);
      try {
        const r = await fetch(base + '/api/health', { cache: 'no-store', signal: ctl.signal });
        if (!r.ok) throw new Error('HTTP ' + r.status);
        return { ok: true, base };
      } catch (e) {
        return { ok: false, base, error: e.message || String(e) };
      } finally {
        clearTimeout(timer);
      }
    },

    async serverInfo() {
      const base = await AE.base();
      try {
        const r = await fetch(base + '/api/extension/info', { cache: 'no-store' });
        if (!r.ok) throw new Error('HTTP ' + r.status);
        return Object.assign({ base }, await r.json());
      } catch (e) {
        return null;
      }
    },

    /** Kick off an extraction on the local server. Returns { taskId }. */
    async extract(opts) {
      const base = await AE.base();
      const s = await AE.settings();
      const body = {
        url: opts.url,
        output: AE.safeName(opts.output || ''),
        format: opts.format || s.format || 'mp3',
        bitrate: Number(opts.bitrate) || s.bitrate || 320,
      };
      const r = await fetch(base + '/api/extract', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(body),
      });
      const data = await r.json().catch(() => ({}));
      if (!r.ok || !data.ok) throw new Error(data.error || ('HTTP ' + r.status));
      return data;
    },

    async tasks() {
      const base = await AE.base();
      const r = await fetch(base + '/api/tasks', { cache: 'no-store' });
      if (!r.ok) throw new Error('HTTP ' + r.status);
      const d = await r.json();
      return Array.isArray(d.tasks) ? d.tasks : [];
    },

    async findTask(id) {
      const list = await AE.tasks();
      return list.find((t) => t.id === id) || null;
    },

    /** Windows/Linux filenames cannot contain these characters. */
    safeName(raw) {
      let s = String(raw || '').replace(/[\\/:*?"<>|]+/g, ' ')
        .replace(/[\u0000-\u001f]+/g, ' ')
        .replace(/\s+/g, ' ')
        .trim();
      if (s.length > 80) s = s.slice(0, 80).trim();
      return s;
    },

    /** Turn a page/<audio> title into something usable as a track name. */
    suggestName(title, url) {
      let s = AE.safeName(title || '');
      if (s) {
        s = s.replace(/\s*[-–—|]\s*(网易云音乐|QQ音乐|酷狗音乐|酷我音乐|咪咕音乐|虾米音乐|bilibili|哔哩哔哩|B站|网易音乐)\s*$/i, '');
        s = s.replace(/\s*[（(](高清|HQ|SQ|MV|Live|官方版|完整版|纯音乐|伴奏)[^）)]*[）)]\s*$/i, '');
        s = s.replace(/\s*[-–—|]\s*(在线观看|在线试听|免费下载|无损音质|MV)\s*$/i, '');
        s = s.trim();
      }
      if (!s && url) {
        try {
          const seg = decodeURIComponent(new URL(url).pathname.split('/').pop() || '');
          s = AE.safeName(seg.replace(/\.(mp3|m4a|aac|flac|wav|ogg|opus|webm|mp4|m4s|ts)$/i, ''));
        } catch (e) { /* ignore */ }
      }
      return s || 'extracted-track';
    },

    stageLabel(stage) {
      switch (stage) {
        case 'queued': return '排队中';
        case 'starting': return '准备中';
        case 'downloading': return '下载中';
        case 'transcoding': return '转码中';
        default: return '处理中';
      }
    },
  };

  scope.AE = AE;
})(typeof self !== 'undefined' ? self : this);
