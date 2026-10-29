/* 无头验证：播放分类列表 + 局域网面板 */
const { chromium } = require('D:/AI/hermes/hermes-agent/node_modules/playwright-core');

const CHROME = 'D:/AI/hermes/tools/chromium-1208/chrome-win64/chrome.exe';
const BASE = 'http://127.0.0.1:8080/';

const pass = [], fail = [];
const ok = (n, c, extra) => (c ? pass : fail).push(n + (extra ? ' → ' + extra : ''));

(async () => {
  const browser = await chromium.launch({ executablePath: CHROME, headless: true });
  const page = await browser.newPage({ viewport: { width: 1440, height: 900 } });
  const errors = [];
  page.on('console', (m) => { if (m.type() === 'error') errors.push(m.text()); });
  page.on('pageerror', (e) => errors.push('pageerror: ' + e.message));

  await page.goto(BASE, { waitUntil: 'networkidle' });
  await page.waitForTimeout(1200);

  // ---- 1. 分类面板渲染 ----
  const trackCount = await page.$$eval('.track', (n) => n.length);
  const allNum = await page.textContent('#catNumAll');
  ok('分类计数「全部曲目」= 列表行数', Number(allNum) === trackCount, `${allNum} vs ${trackCount}`);

  const albums = await page.$$eval('#catAlbums .cat-item', (n) => n.length);
  const artists = await page.$$eval('#catArtists .cat-item', (n) => n.length);
  ok('生成「按专辑」分类', albums > 0, albums + ' 个');
  ok('生成「按歌手」分类', artists > 0, artists + ' 个');

  const names = await page.$$eval('#catArtists .cat-item .cat-name', (n) => n.map((x) => x.textContent));
  ok('歌手分类名有内容', names.length > 0 && names[0].length > 0, names.slice(0, 3).join(' / '));

  // ---- 2. 点击歌手分类过滤 ----
  const targetArtist = await page.$eval('#catArtists .cat-item', (b) => b.dataset.value);
  await page.click('#catArtists .cat-item');
  await page.waitForTimeout(400);
  const filteredCount = await page.$$eval('.track', (n) => n.length);
  const chipVisible = await page.isVisible('#catChip');
  const chipText = await page.textContent('#catChipText');
  ok('点击歌手后列表被过滤', filteredCount > 0 && filteredCount < trackCount,
     `${trackCount} → ${filteredCount}`);
  ok('顶部显示当前分类 chip', chipVisible && chipText === targetArtist, chipText);

  const rowsArtist = await page.$$eval('.track .track-artist', (n) => n.map((x) => x.textContent.trim()));
  ok('过滤结果只含该歌手', rowsArtist.every((a) => a === targetArtist),
     rowsArtist.slice(0, 3).join(' / '));

  // active 高亮
  const actives = await page.$$eval('.cat-item.is-active', (n) => n.length);
  ok('当前分类有且仅有一个高亮', actives === 1, String(actives));

  // ---- 3. 专辑分类切换 ----
  await page.click('#catAlbums .cat-item');
  await page.waitForTimeout(400);
  const albumRows = await page.$$eval('.track', (n) => n.length);
  ok('切换到专辑分类后仍有曲目', albumRows > 0, albumRows + ' 首');

  // ---- 4. 返回全部 ----
  await page.click('#catChipClear');
  await page.waitForTimeout(400);
  const backCount = await page.$$eval('.track', (n) => n.length);
  const chipHidden = await page.isHidden('#catChip');
  ok('返回全部恢复列表', backCount === trackCount, `${backCount} vs ${trackCount}`);
  ok('chip 隐藏', chipHidden);

  // ---- 5. 收藏：行内 ♥ ----
  await page.click('.track:first-child .op-fav');
  await page.waitForTimeout(600);
  const favNum = Number(await page.textContent('#catNumFav'));
  ok('♥ 收藏计数 +1', favNum === 1, String(favNum));

  const serverFavs = await page.evaluate(async () => (await fetch('/api/favorites')).json());
  ok('收藏已写入服务端', serverFavs.ids.length === 1, JSON.stringify(serverFavs.ids).slice(0, 40));

  const favActive = await page.$$eval('.op-fav.is-on', (n) => n.length);
  ok('该行 ♥ 点亮', favActive === 1, String(favActive));

  // 刷新后仍保留
  await page.reload({ waitUntil: 'networkidle' });
  await page.waitForTimeout(1000);
  const favAfterReload = Number(await page.textContent('#catNumFav'));
  ok('刷新页面收藏仍在', favAfterReload === 1, String(favAfterReload));

  // ---- 6. 收藏分类视图 ----
  await page.click('.cat-item[data-cat="fav"]');
  await page.waitForTimeout(500);
  const favRows = await page.$$eval('.track', (n) => n.length);
  const favChip = await page.textContent('#catChipText');
  ok('「我的收藏」只显示收藏曲目', favRows === 1, favRows + ' 首');
  ok('收藏分类 chip 文案正确', favChip.trim() === '我的收藏', favChip);

  // ---- 7. 取消收藏后的空态 ----
  await page.click('.track:first-child .op-fav');
  await page.waitForTimeout(600);
  const catEmptyShown = await page.isVisible('#emptyCat');
  const catEmptyTitle = await page.textContent('#emptyCatTitle');
  ok('收藏清空后显示分类空态', catEmptyShown, catEmptyTitle.trim());
  await page.click('#emptyCatBack');
  await page.waitForTimeout(400);
  const afterBack = await page.$$eval('.track', (n) => n.length);
  ok('空态「返回全部」生效', afterBack === trackCount, String(afterBack));

  // ---- 8. 右键菜单收藏项 ----
  await page.click('.track:first-child [data-more]');
  await page.waitForTimeout(300);
  const hasFavItem = await page.$$eval('#ctxMenu [data-act="fav"]', (n) => n.length);
  ok('右键菜单含「收藏」', hasFavItem === 1);
  await page.click('#ctxMenu [data-act="fav"]');
  await page.waitForTimeout(500);
  ok('右键收藏生效', Number(await page.textContent('#catNumFav')) === 1,
     await page.textContent('#catNumFav'));
  await page.click('.track:first-child .op-fav');   // 还原
  await page.waitForTimeout(400);

  // ---- 9. 局域网面板 ----
  await page.click('#lanBtn');
  await page.waitForTimeout(700);
  const lanItems = await page.$$eval('#lanList .net-item', (n) => n.length);
  const lanText = await page.textContent('#lanInfo');
  ok('局域网面板列出地址', lanItems >= 2, lanItems + ' 个地址');
  ok('面板显示端口/开放状态', /端口 8080/.test(lanText), lanText.trim());
  const firstUrl = await page.textContent('#lanList .net-item:nth-child(2) .net-url');
  ok('含局域网 IP 地址', /http:\/\/(?!127)\d+\.\d+/.test(firstUrl), firstUrl.trim());
  await page.click('#lanClose');
  await page.waitForTimeout(200);
  ok('面板可关闭', await page.isHidden('#lanModal'));

  // ---- 10. 浅色主题下无异常 ----
  await page.click('#themeBtn');
  await page.waitForTimeout(400);
  const themedInAll = await page.isVisible('#catArtists .cat-item');
  ok('浅色主题下分类仍可见', themedInAll, await page.getAttribute('html', 'data-theme'));
  await page.click('#themeBtn');

  // ---- 11. 窄屏（手机）抽屉 ----
  await page.setViewportSize({ width: 420, height: 860 });
  await page.waitForTimeout(500);
  ok('窄屏出现菜单按钮', await page.isVisible('#menuFab'));
  ok('抽屉默认收起', await page.isHidden('#drawerBackdrop'));
  await page.click('#menuFab');
  await page.waitForTimeout(500);
  ok('点击后抽屉展开并可见分类', await page.isVisible('#catArtists .cat-item'));
  await page.click('#catArtists .cat-item');
  await page.waitForTimeout(500);
  ok('选择分类后抽屉自动收起', await page.isHidden('#drawerBackdrop'));
  const narrowRows = await page.$$eval('.track', (n) => n.length);
  ok('窄屏下过滤仍生效', narrowRows > 0, narrowRows + ' 首');
  ok('窄屏仍能看到 ♥', await page.isVisible('.track:first-child .op-fav'));
  await page.click('#catChipClear');
  await page.waitForTimeout(300);
  await page.setViewportSize({ width: 1440, height: 900 });
  await page.waitForTimeout(300);

  // ---- 12. 新建播放列表 ----
  await page.click('#addPlaylistBtn');
  await page.waitForTimeout(400);
  ok('「添加列表」按钮打开新建弹窗', await page.isVisible('#plModal'));
  await page.fill('#plName', '开车听');
  await page.click('#plCreate');
  await page.waitForTimeout(700);
  const plCount = await page.$$eval('#catPlaylists .cat-item', (n) => n.length);
  ok('新列表出现在侧栏', plCount === 1, plCount + ' 个');
  ok('列表名正确', (await page.textContent('#catPlaylists .cat-name')).trim() === '开车听');

  // ---- 13. 更多操作 → 添加到列表 ----
  await page.click('.track:first-child [data-more]');
  await page.waitForTimeout(300);
  ok('菜单含「添加到列表」', await page.isVisible('#ctxMenu [data-act="addto"]'));
  await page.click('#ctxMenu [data-act="addto"]');
  await page.waitForTimeout(500);
  ok('打开「添加到列表」弹窗', await page.isVisible('#addToModal'));
  const addToInfo = await page.textContent('#addToInfo');
  ok('弹窗显示目标曲目', addToInfo.trim().length > 0, addToInfo.trim());

  await page.click('#addToList [data-pl]');
  await page.waitForTimeout(700);
  const plRowsOn = await page.$$eval('#addToList .pl-row.is-on', (n) => n.length);
  ok('勾选后列表项变为已加入', plRowsOn === 1, String(plRowsOn));
  const plBadge = await page.textContent('#catPlaylists .cat-num');
  ok('侧栏计数 +1', plBadge.trim() === '1', plBadge.trim());
  await page.click('#addToDone');
  await page.waitForTimeout(300);

  // 服务端持久化
  const serverPls = await page.evaluate(async () => (await fetch('/api/playlists')).json());
  ok('列表已写入服务端', serverPls.playlists.length === 1 && serverPls.playlists[0].ids.length === 1,
     JSON.stringify(serverPls.playlists[0] && { name: serverPls.playlists[0].name, n: serverPls.playlists[0].ids.length }));

  // ---- 14. 打开列表分类 → 只显示列表内曲目 ----
  await page.click('#catPlaylists .cat-item');
  await page.waitForTimeout(600);
  const plViewRows = await page.$$eval('.track', (n) => n.length);
  const plChip = await page.textContent('#catChipText');
  ok('列表分类只显示列表内曲目', plViewRows === 1, plViewRows + ' 首');
  ok('chip 显示列表名', plChip.trim() === '开车听', plChip.trim());

  // 从列表里再点一次 → 移除 → 空态
  await page.click('.track:first-child [data-more]');
  await page.waitForTimeout(300);
  await page.click('#ctxMenu [data-act="addto"]');
  await page.waitForTimeout(400);
  await page.click('#addToList [data-pl]');
  await page.waitForTimeout(600);
  await page.click('#addToDone');
  await page.waitForTimeout(400);
  ok('取消勾选后列表为空并显示空态', await page.isVisible('#emptyCat'),
     (await page.textContent('#emptyCatTitle')).trim());
  await page.click('#emptyCatBack');
  await page.waitForTimeout(400);

  // ---- 15. 刷新后列表仍在 ----
  await page.reload({ waitUntil: 'networkidle' });
  await page.waitForTimeout(1200);
  ok('刷新后列表仍在', (await page.$$eval('#catPlaylists .cat-item', (n) => n.length)) === 1);

  // ---- 16. 删除列表 ----
  page.on('dialog', (d) => d.accept());
  await page.hover('#catPlaylists .cat-wrap');
  await page.click('#catPlaylists [data-delpl]');
  await page.waitForTimeout(700);
  ok('删除列表后侧栏清空', (await page.$$eval('#catPlaylists .cat-item', (n) => n.length)) === 0);

  ok('全程控制台无报错', errors.length === 0, errors.slice(0, 3).join(' | '));

  await browser.close();
  console.log('通过 ' + pass.length + ' / 失败 ' + fail.length);
  pass.forEach((p) => console.log('  PASS  ' + p));
  fail.forEach((f) => console.log('  FAIL  ' + f));
  process.exit(fail.length ? 1 : 0);
})().catch((e) => {
  console.error('运行异常:', e.message);
  console.log('已通过 ' + pass.length + ' 项:');
  pass.forEach((p) => console.log('  PASS  ' + p));
  fail.forEach((f) => console.log('  FAIL  ' + f));
  process.exit(2);
});
