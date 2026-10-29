/* ===================================================================
 * Background service worker — owns the context menus and performs the
 * extraction request so a right-click works without opening the popup.
 * =================================================================== */
importScripts('api.js');

const MENU_LINK = 'ae-extract-link';
const MENU_PAGE = 'ae-extract-page';
const MENU_OPEN = 'ae-open-player';

function notify(text) {
  try {
    chrome.notifications.create('', {
      type: 'basic',
      iconUrl: 'icons/icon48.png',
      title: '音乐提取',
      message: String(text).slice(0, 180),
    });
  } catch (e) {
    /* notification API unavailable — nothing else we can do here */
  }
}

async function submit(url, title) {
  const s = await AE.settings();
  const name = AE.suggestName(title, url);
  const res = await AE.extract({ url, output: name, format: s.format, bitrate: s.bitrate });
  try {
    await chrome.storage.local.set({ aeLastTaskId: res.taskId, aeLastTaskName: name });
  } catch (e) { /* ignore */ }
  return name;
}

chrome.runtime.onInstalled.addListener(() => {
  chrome.contextMenus.removeAll(() => {
    chrome.contextMenus.create({ id: MENU_LINK, title: '提取该链接到曲库', contexts: ['link'] });
    chrome.contextMenus.create({ id: MENU_PAGE, title: '提取本页正在播放的歌到曲库', contexts: ['page', 'audio', 'video'] });
    chrome.contextMenus.create({ id: MENU_OPEN, title: '打开本地音乐播放器', contexts: ['action'] });
  });
});

chrome.contextMenus.onClicked.addListener(async (info, tab) => {
  if (info.menuItemId === MENU_OPEN) {
    const base = await AE.base();
    chrome.tabs.create({ url: base + '/' });
    return;
  }
  const url = info.menuItemId === MENU_LINK
    ? (info.linkUrl || info.srcUrl || (tab && tab.url))
    : (tab && tab.url);
  if (!url) { notify('这个位置没有可提取的链接'); return; }
  try {
    const name = await submit(url, (tab && tab.title) || '');
    notify('已开始提取：' + name);
  } catch (e) {
    const msg = e && e.message ? e.message : String(e);
    notify(/fetch|Failed|NetworkError|HTTP/i.test(msg)
      ? '连不上本地服务，请先启动 music_player'
      : '提取失败：' + msg);
  }
});

/* Let the popup delegate a submit here too (it keeps the badge in sync). */
chrome.runtime.onMessage.addListener((msg, sender, sendResponse) => {
  if (!msg || msg.type !== 'ae-background-extract') return undefined;
  submit(msg.url, msg.title)
    .then((name) => sendResponse({ ok: true, name }))
    .catch((e) => sendResponse({ ok: false, error: (e && e.message) || String(e) }));
  return true;
});
