/* =====================================================================
 * 无头验证：浏览器扩展「抓歌到曲库」端到端链路
 *
 * 流程：加载 unpacked 扩展 → 打开一个自动播放 mp3 的测试页 → 以标签页
 * 方式打开扩展 popup（mock chrome.tabs.query 指向测试页）→ 断言候选检测
 * → 点击「提取到全部曲目」→ 等待任务完成 → 校验曲目进入 /api/library
 * → 校验 Web UI 的插件安装引导面板。
 * ===================================================================== */
const { chromium } = require('D:/AI/hermes/hermes-agent/node_modules/playwright-core');
const http = require('http');
const fs = require('fs');
const os = require('os');
const path = require('path');

const CHROME = 'D:/AI/hermes/tools/chromium-1208/chrome-win64/chrome.exe';
const ROOT = 'D:/AI/agents/audio/audio-extractor';
const EXT_DIR = ROOT + '/build/extension';
const MUSICES = ROOT + '/build/musices';
const BASE = 'http://127.0.0.1:8080';
const PROXY_PORT = 18080;

const pass = [], fail = [];
const ok = (n, c, extra) => (c ? pass : fail).push(n + (extra ? ' → ' + extra : ''));

(async () => {
  // ---------- 1. 本地测试站：自动播放曲库里的第一个 mp3 ----------
  const mp3s = fs.readdirSync(MUSICES).filter((f) => f.toLowerCase().endsWith('.mp3'));
  if (!mp3s.length) throw new Error('曲库里没有 mp3 可用于测试');
  const srcMp3 = path.join(MUSICES, mp3s[0]);

  const server = http.createServer((req, res) => {
    if (req.url.startsWith('/test.mp3')) {
      res.writeHead(200, { 'Content-Type': 'audio/mpeg' });
      fs.createReadStream(srcMp3).pipe(res);
      return;
    }
    res.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8' });
    res.end('<!doctype html><meta charset="utf-8"><title>测试歌曲 - 无头验证</title>'
      + '<audio id="a" autoplay muted loop src="/test.mp3"></audio>');
  });
  await new Promise((r) => server.listen(PROXY_PORT, r));

  const before = await (await fetch(BASE + '/api/library?refresh=1')).json();
  const countBefore = before.count;

  // ---------- 2. 加载扩展 ----------
  const userDataDir = fs.mkdtempSync(path.join(os.tmpdir(), 'ae-ext-'));
  const ctx = await chromium.launchPersistentContext(userDataDir, {
    executablePath: CHROME,
    headless: false,
    args: [
      '--headless=new',
      '--disable-extensions-except=' + EXT_DIR,
      '--load-extension=' + EXT_DIR,
      '--autoplay-policy=no-user-gesture-required',
      '--no-first-run',
      '--no-default-browser-check',
    ],
  });

  let sw;
  try {
    [sw] = ctx.serviceWorkers();
    if (!sw || !sw.url().startsWith('chrome-extension://')) {
      sw = await ctx.waitForEvent('serviceworker', { timeout: 15000 });
    }
    const extId = new URL(sw.url()).host;
    ok('扩展加载成功（MV3 service worker）', !!extId, extId);

    // ---------- 3. 测试页：audio 正在播放 ----------
    const tab = await ctx.newPage();
    await tab.goto('http://127.0.0.1:' + PROXY_PORT + '/test.html', { waitUntil: 'load' });
    await tab.waitForFunction(() => {
      const a = document.getElementById('a');
      return a && !a.paused && a.readyState >= 3;
    }, { timeout: 15000 }).catch(() => {});
    const playing = await tab.evaluate(() => {
      const a = document.getElementById('a');
      return a ? (!a.paused && a.readyState >= 3) : false;
    });
    ok('测试页音频进入播放状态', playing);
    const tabId = await (async () => {
      // chrome.tabs is not exposed to content pages; get the id via the SW.
      return await sw.evaluate((port) => new Promise((res) => {
        chrome.tabs.query({ url: 'http://127.0.0.1:' + port + '/*' }, (t) => res(t[0] ? t[0].id : -1));
      }), PROXY_PORT);
    })();
    ok('定位测试页 tabId', tabId > 0, String(tabId));

    // ---------- 4. popup 完整流程 ----------
    const popup = await ctx.newPage();
    const perr = [];
    popup.on('pageerror', (e) => perr.push(e.message));
    await popup.addInitScript((id) => {
      chrome.tabs.query = async () => [{ id, url: 'http://test/', title: '测试歌曲 - 无头验证' }];
    }, tabId);
    await popup.goto('chrome-extension://' + extId + '/popup.html');
    await popup.waitForTimeout(1200);

    const srv = await popup.textContent('#srvState');
    ok('popup 检测到本地服务', srv.includes('已连接'), srv.trim());
    const candCount = await popup.$$eval('#cands .cand', (n) => n.length);
    ok('检测到候选音源', candCount >= 1, candCount + ' 个');
    const hasPlayingTag = await popup.$$eval('#cands .tag', (n) => n.some((x) => x.textContent === '正在播放'));
    ok('候选带「正在播放」徽标', hasPlayingTag);
    const name = await popup.inputValue('#name');
    ok('自动填充保存名', name.length > 0, name);

    await popup.fill('#name', '扩展验证曲');
    await popup.click('#btnGo');
    await popup.waitForSelector('#foot:not([hidden])', { timeout: 120000 });
    const foot = await popup.textContent('#footText');
    ok('提取任务完成', foot.includes('扩展验证曲'), foot.trim());
    const barW = await popup.$eval('#barFill', (n) => n.style.width);
    ok('进度条到达 100%', barW === '100%', barW);

    // ---------- 5. 曲库校验 ----------
    await new Promise((r) => setTimeout(r, 1500));
    const after = await (await fetch(BASE + '/api/library?refresh=1')).json();
    const added = after.tracks.find((t) => t.title === '扩展验证曲' || t.fileName === '扩展验证曲.mp3');
    ok('新曲目出现在「全部曲目」', !!added, added ? added.fileName : '未找到');

    // ---------- 6. Web UI 插件引导面板 ----------
    const ui = await ctx.newPage();
    const errors = [];
    ui.on('pageerror', (e) => errors.push(e.message));
    await ui.goto(BASE + '/', { waitUntil: 'networkidle' });
    await ui.click('#extBtn');
    await ui.waitForSelector('#extModal:not([hidden])');
    const stCls = await ui.$eval('#extStatus', (n) => n.className);
    const dir = await ui.textContent('#extDir');
    const steps = await ui.$$eval('.ext-steps li', (n) => n.length);
    ok('插件面板显示就绪状态', stCls.includes('is-ok'), stCls);
    ok('显示扩展目录路径', dir.includes('extension'), dir.trim());
    ok('安装步骤齐全', steps === 4, steps + ' 步');
    ok('Web UI 无脚本报错', errors.length === 0, errors.slice(0, 2).join(' | '));
    ok('popup 无脚本报错', perr.length === 0, perr.slice(0, 2).join(' | '));

    // ---------- cleanup ----------
    if (added) {
      try { fs.unlinkSync(path.join(MUSICES, added.fileName)); } catch (e) { /* ignore */ }
      await fetch(BASE + '/api/library?refresh=1');
    }
  } catch (e) {
    fail.push('异常中断 → ' + (e && e.message ? e.message : String(e)));
  } finally {
    await ctx.close().catch(() => {});
    server.close();
    fs.rmSync(userDataDir, { recursive: true, force: true });
  }

  console.log('\n===== 扩展端到端验证 =====');
  console.log('通过 ' + pass.length + ' 项:');
  pass.forEach((p) => console.log('  ✓ ' + p));
  if (fail.length) {
    console.log('失败 ' + fail.length + ' 项:');
    fail.forEach((f) => console.log('  ✗ ' + f));
    process.exit(1);
  }
  console.log('全部通过');
})();
