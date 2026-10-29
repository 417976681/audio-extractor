/* =====================================================================
 * 无头验证：删除曲目（连同歌词）并刷新所有列表
 *
 * 用独立的临时曲库 build/testlib 跑，绝不碰用户的真实歌曲：
 *   1. 复制两首 mp3 + 给其中一首写 .lrc
 *   2. 收藏这首、并塞进一个播放列表
 *   3. 播放它 → 右键「删除…」→ 断言确认框内容（含歌词提示）
 *   4. 取消 → 文件仍在
 *   5. 再次删除并确认 → 断言文件 + .lrc 都消失，收藏/列表/曲库/播放器全部刷新
 * ===================================================================== */
const { chromium } = require('D:/AI/hermes/hermes-agent/node_modules/playwright-core');
const fs = require('fs');
const path = require('path');

const CHROME = 'D:/AI/hermes/tools/chromium-1208/chrome-win64/chrome.exe';
const ROOT = 'D:/AI/agents/audio/audio-extractor';
const MUSICES = ROOT + '/build/musices';
const TESTLIB = ROOT + '/build/testlib';
const BASE = 'http://127.0.0.1:8080';

const pass = [], fail = [];
const ok = (n, c, extra) => (c ? pass : fail).push(n + (extra ? ' → ' + extra : ''));

const jpost = (url, body) => fetch(BASE + url, {
  method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body),
}).then((r) => r.json());

(async () => {
  // ---------------------------------------------------------------- setup
  const srcMp3 = fs.readdirSync(MUSICES).find((f) => f.toLowerCase().endsWith('.mp3'));
  if (!srcMp3) throw new Error('曲库里没有 mp3 可用作测试素材');
  fs.rmSync(TESTLIB, { recursive: true, force: true });
  fs.mkdirSync(TESTLIB, { recursive: true });
  const fileA = path.join(TESTLIB, '测试曲A.mp3');
  const fileB = path.join(TESTLIB, '测试曲B.mp3');
  const lrcA = path.join(TESTLIB, '测试曲A.lrc');
  fs.copyFileSync(path.join(MUSICES, srcMp3), fileA);
  fs.copyFileSync(path.join(MUSICES, srcMp3), fileB);
  fs.writeFileSync(lrcA, '[00:00.00]第一行歌词\n[00:05.00]第二行歌词\n', 'utf8');

  await jpost('/api/config', { libraryDir: TESTLIB });
  const lib = await (await fetch(BASE + '/api/library?refresh=1')).json();
  const trackA = lib.tracks.find((t) => t.fileName === '测试曲A.mp3');
  const trackB = lib.tracks.find((t) => t.fileName === '测试曲B.mp3');
  ok('临时曲库就绪（2 首）', lib.count === 2 && trackA && trackB, lib.count + ' 首');
  ok('曲目 JSON 带 hasLyric 标记',
     trackA && trackA.hasLyric === true && trackB && trackB.hasLyric === false,
     'A=' + (trackA && trackA.hasLyric) + ' B=' + (trackB && trackB.hasLyric));

  // Clear leftovers from an interrupted run (same-name lists are rejected).
  const existing = await (await fetch(BASE + '/api/playlists')).json();
  for (const p of existing.playlists) {
    if (p.name === '删除测试列表') {
      await fetch(BASE + '/api/playlists?id=' + p.id, { method: 'DELETE' });
    }
  }
  await jpost('/api/favorites', { id: trackA.id, on: true });
  const plRes = await jpost('/api/playlists', { name: '删除测试列表' });
  const plId = plRes.playlist.id;
  await jpost('/api/playlists/tracks', { playlistId: plId, trackId: trackA.id });

  const browser = await chromium.launch({ executablePath: CHROME, headless: true });
  const page = await browser.newPage({ viewport: { width: 1440, height: 900 } });
  const errors = [];
  page.on('pageerror', (e) => errors.push(e.message));
  page.on('console', (m) => { if (m.type() === 'error') errors.push(m.text()); });

  try {
    await page.goto(BASE + '/', { waitUntil: 'networkidle' });
    await page.waitForTimeout(1200);

    const rows = () => page.$$eval('.track', (n) => n.length);
    ok('列表载入 2 首', (await rows()) === 2);
    ok('收藏计数为 1', (await page.textContent('#catNumFav')).trim() === '1');
    const plBadge = (await page.textContent('#catPlaylists .cat-num')).trim();
    ok('播放列表计数为 1', plBadge === '1', plBadge);

    // ---- 播放 A（让它成为"正在播放"） ----
    const idxA = await page.$$eval('.track', (n) => n.findIndex((r) => r.textContent.includes('测试曲A')));
    await page.click(`.track:nth-child(${idxA + 1})`);
    await page.waitForTimeout(900);
    ok('开始播放测试曲A', (await page.textContent('#npTitle')).includes('测试曲A'),
       await page.textContent('#npTitle'));

    // ---- 「更多操作」→ 删除… ----
    await page.click(`.track:nth-child(${idxA + 1}) [data-more]`);
    await page.waitForTimeout(300);
    const delBtn = page.locator('#ctxMenu [data-act="delete"]');
    ok('「更多操作」菜单含「删除…」', await delBtn.isVisible());
    ok('菜单项为危险色样式', (await delBtn.getAttribute('class') || '').includes('danger'));
    await delBtn.click();
    await page.waitForSelector('#delModal:not([hidden])');
    ok('弹出删除确认框', await page.isVisible('#delModal'));
    ok('确认框显示曲名', (await page.textContent('#delTitle')).includes('测试曲A'));
    ok('确认框显示文件信息', (await page.textContent('#delPath')).includes('.mp3'));
    ok('提示将同时删除歌词', await page.isVisible('#delLrcRow'),
       await page.textContent('#delLrcName'));
    ok('提示文案含不可撤销', (await page.textContent('.del-warn')).includes('不可撤销'));

    // ---- 取消：文件必须还在 ----
    await page.click('#delCancel');
    await page.waitForTimeout(300);
    ok('取消后弹窗关闭', await page.isHidden('#delModal'));
    ok('取消后音频文件未删', fs.existsSync(fileA));

    // ---- 再来一次，确认删除 ----
    await page.click(`.track:nth-child(${idxA + 1}) [data-more]`);
    await page.waitForTimeout(300);
    await page.click('#ctxMenu [data-act="delete"]');
    await page.waitForSelector('#delModal:not([hidden])');
    await page.click('#delConfirm');
    await page.waitForTimeout(2500);

    ok('音频文件已从磁盘删除', !fs.existsSync(fileA));
    ok('同名歌词文件已删除', !fs.existsSync(lrcA));
    ok('另一首未受影响', fs.existsSync(fileB));

    // ---- 所有列表刷新 ----
    ok('列表行数减为 1', (await rows()) === 1, (await rows()) + ' 行');
    ok('侧栏曲目计数更新', (await page.textContent('#trackCount')).trim() === '1',
       await page.textContent('#trackCount'));
    ok('收藏计数归零', (await page.textContent('#catNumFav')).trim() === '0',
       await page.textContent('#catNumFav'));
    const plBadge2 = (await page.textContent('#catPlaylists .cat-num')).trim();
    ok('播放列表计数归零', plBadge2 === '0', plBadge2);
    ok('被删曲目已从播放器移出', (await page.textContent('#npTitle')).trim() === '未在播放',
       await page.textContent('#npTitle'));
    ok('删除提示包含「含歌词」', (await page.textContent('#toast')).includes('含歌词'),
       await page.textContent('#toast'));

    const favs = await (await fetch(BASE + '/api/favorites')).json();
    ok('服务端收藏已清理', !favs.ids.includes(trackA.id), JSON.stringify(favs.ids));
    const pls = await (await fetch(BASE + '/api/playlists')).json();
    const target = pls.playlists.find((p) => p.id === plId);
    ok('服务端列表已清理该曲', target && target.count === 0, target ? String(target.count) : '列表丢失');

    // ---- 刷新页面后依然一致（持久化） ----
    await page.reload({ waitUntil: 'networkidle' });
    await page.waitForTimeout(1000);
    ok('刷新后仍只有 1 首', (await page.$$eval('.track', (n) => n.length)) === 1);

    ok('全程无脚本报错', errors.length === 0, errors.slice(0, 2).join(' | '));

    // ---- 清理 ----
    await fetch(BASE + '/api/playlists?id=' + plId, { method: 'DELETE' });
  } finally {
    await browser.close();
    await jpost('/api/config', { libraryDir: MUSICES });
    fs.rmSync(TESTLIB, { recursive: true, force: true });
  }

  console.log('\n===== 删除曲目验证 =====');
  console.log('通过 ' + pass.length + ' / 失败 ' + fail.length);
  pass.forEach((p) => console.log('  PASS  ' + p));
  fail.forEach((f) => console.log('  FAIL  ' + f));
  process.exit(fail.length ? 1 : 0);
})().catch((e) => {
  console.error('运行异常:', e.message);
  console.log('已通过 ' + pass.length + ' 项');
  pass.forEach((p) => console.log('  PASS  ' + p));
  fail.forEach((f) => console.log('  FAIL  ' + f));
  // 兜底把曲库切回真实目录
  jpost('/api/config', { libraryDir: MUSICES }).catch(() => {});
  process.exit(2);
});
