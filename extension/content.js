/* ===================================================================
 * Content script — finds whatever the current tab is playing so the
 * popup can offer it for extraction.
 *
 * Three independent detection strategies, because every site hides its
 * audio differently:
 *   1. <audio> / <video> elements that are actually playing
 *   2. HTTP requests recorded by the performance timeline (works for
 *      streams that never surface an element src, e.g. MSE feeds)
 *   3. og:audio / og:title metadata, plus the page URL itself as the
 *      last resort (the server can resolve page links for many sites)
 * =================================================================== */
(function () {
  'use strict';

  const MEDIA_EXT = /\.(mp3|m4a|aac|flac|wav|ogg|oga|opus|weba|webm|mkv|mp4|m4v|m4s|mov|ts)(\?|#|$)/i;

  function absolutize(raw) {
    if (!raw) return '';
    try {
      return new URL(raw, location.href).href;
    } catch (e) {
      return '';
    }
  }

  function fileNameOf(url) {
    try {
      const u = new URL(url);
      const seg = decodeURIComponent(u.pathname.split('/').pop() || '');
      return seg || u.hostname;
    } catch (e) {
      return String(url).slice(0, 60);
    }
  }

  function metaContent(selector, attr) {
    const node = document.querySelector(selector);
    return node ? (node.getAttribute(attr || 'content') || '').trim() : '';
  }

  function trimUrl(u, max) {
    const s = String(u || '');
    return s.length > max ? s.slice(0, max - 1) + '…' : s;
  }

  /** Strategy 1: media elements currently attached to the document. */
  function fromElements() {
    const out = [];
    document.querySelectorAll('audio,video').forEach((el, i) => {
      const raw = el.currentSrc || el.getAttribute('src') || '';
      const url = absolutize(raw);
      if (!url) return;
      const playing = !el.paused && !el.ended && el.readyState > 2;
      const dur = Number.isFinite(el.duration) && el.duration > 0 ? el.duration : 0;
      out.push({
        id: 'el' + i,
        kind: 'element',
        url,
        usable: !/^blob:/i.test(url),
        playing,
        duration: dur,
        size: 0,
        title: fileNameOf(url),
        note: playing ? '正在播放' : (dur ? '已加载' : '未播放'),
      });
    });
    return out;
  }

  /** Strategy 2: media requests seen by the performance timeline. */
  function fromNetwork() {
    let entries = [];
    try {
      entries = performance.getEntriesByType('resource') || [];
    } catch (e) {
      entries = [];
    }
    const out = [];
    entries.forEach((r, i) => {
      const name = r.name || '';
      if (!/^https?:/i.test(name)) return;
      const itype = r.initiatorType || '';
      const isMediaInitiator = itype === 'audio' || itype === 'video';
      const looksMedia = MEDIA_EXT.test(name) || /\.(m3u8)(\?|$)/i.test(name);
      if (!isMediaInitiator && !looksMedia) return;
      const size = Number(r.transferSize || r.encodedBodySize || r.decodedBodySize || 0);
      // Tiny files are UI sprites / pings rather than real audio payloads.
      if (!isMediaInitiator && size > 0 && size < 48 * 1024) return;
      out.push({
        id: 'net' + i,
        kind: 'network',
        url: name,
        usable: true,
        playing: false,
        duration: 0,
        size,
        title: fileNameOf(name),
        note: size ? (size / 1048576).toFixed(1) + ' MB' : '媒体请求',
      });
    });
    out.sort((a, b) => b.size - a.size);
    return out.slice(0, 6);
  }

  /** Strategy 3: page metadata + the page URL itself. */
  function fromPage() {
    const out = [];
    const og = absolutize(metaContent('meta[property="og:audio"]') ||
                          metaContent('meta[name="twitter:audio"]') ||
                          metaContent('meta[name="audio"]'));
    if (og && /^https?:/i.test(og)) {
      out.push({
        id: 'meta0', kind: 'meta', url: og, usable: true, playing: false,
        duration: 0, size: 0, title: fileNameOf(og), note: '页面声明的音频',
      });
    }
    if (/^https?:/i.test(location.href)) {
      out.push({
        id: 'page', kind: 'page', url: location.href, usable: true, playing: false,
        duration: 0, size: 0, title: document.title || location.hostname,
        note: '由服务端解析此页面',
      });
    }
    return out;
  }

  function collect() {
    let result;
    try {
      const media = fromElements();
      const net = fromNetwork();
      const page = fromPage();
      const seen = new Set();
      const candidates = [...media, ...net, ...page].filter((c) => {
        if (!c || !c.url) return false;
        if (/^(data|about|javascript|chrome-extension|file):/i.test(c.url)) return false;
        const key = c.url;
        if (seen.has(key)) return false;
        seen.add(key);
        return true;
      });
      result = {
        ok: true,
        pageUrl: location.href,
        pageTitle: document.title || '',
        host: location.hostname,
        ogTitle: metaContent('meta[property="og:title"]'),
        candidates,
        hasPlaying: candidates.some((c) => c.playing),
        hasBlob: media.some((m) => /^blob:/i.test(m.url)),
      };
    } catch (e) {
      result = { ok: false, error: String((e && e.message) || e), candidates: [] };
    }
    return result;
  }

  chrome.runtime.onMessage.addListener((msg, sender, sendResponse) => {
    if (!msg || msg.type !== 'ae-collect') return undefined;
    try {
      sendResponse(collect());
    } catch (e) {
      sendResponse({ ok: false, error: String(e), candidates: [] });
    }
    return undefined;
  });
})();
