# Audio Extractor · 使用文档

一个用 C++ 编写的音视频提取 + 本地音乐播放器工具。它可以从视频网站、直播源、直链中下载音频并转码成 MP3 / WAV，自动归入本地曲库，然后在内置的 Web 界面里播放、管理、联网抓歌词，还能让同一局域网内的手机 / 平板 / 电脑一起访问。

> 配套浏览器插件可以把网页里**正在播放**的歌曲一键抓进曲库。

---

## 一、功能一览

- **多平台音频提取**：B 站（含 b23.tv 短链）、YouTube、抖音、HLS/m3u8 直播源、普通音视频直链。
- **命令行与图形界面双形态**：`audio_extractor`（纯命令行）做批量/脚本化提取；`music_player`（本地 Web 服务 + 浏览器界面）做日常听歌与管理。
- **自动联网歌词**：命中后存为与音频同名的 `.lrc` 文件，下次播放免联网；支持多版本选择与整体时间偏移。
- **局域网访问**：默认对所有网卡开放，手机、平板、其它电脑打开地址即可使用；界面内可随时查看访问地址。
- **曲库管理**：按「全部曲目 / 我的收藏 / 自建播放列表 / 按专辑 / 按歌手」分类；支持搜索、排序、新建/删除播放列表、把歌曲加入指定列表、收藏、删除（同时删歌词）。
- **浏览器插件**：检测到网页中正在播放的 `<audio>/<video>` / 网络媒体，点一下即可下载转码并归入曲库；也支持在任意链接上右键「提取该链接到曲库」。
- **中文友好**：全程 UTF-8，文件路径、歌名、列表名含中文均无压力。

---

## 二、支持哪些链接

| 类型 | 示例 | 说明 |
| --- | --- | --- |
| B 站视频 | `https://www.bilibili.com/video/BV1xx411c7XD` | 支持 BV 号、av 号 |
| B 站短链 | `https://b23.tv/AbCdEf` | 自动展开为真实页面 |
| YouTube 视频 | `https://www.youtube.com/watch?v=ID`、`https://youtu.be/ID`、`/embed/ID`、`/shorts/ID`、`/v/ID` | 仅识别含 11 位视频 ID 的真实视频页 |
| 抖音 | `https://v.douyin.com/XXXX/`（短链）、`https://www.douyin.com/video/<aweme_id>`、`?modal_id=<id>` | 见下方「已知限制」 |
| HLS / m3u8 | `https://.../playlist.m3u8` | 直播源或点播切片均可 |
| 直链媒体 | 任意 `.mp3/.m4a/.mp4/...` 的直接下载地址 | 普通文件直链 |

---

## 三、编译与构建

### 3.1 依赖

| 依赖 | 用途 | 获取方式 |
| --- | --- | --- |
| FFmpeg（`libavformat/libavcodec/libavutil/libswresample` 等） | 解码、转码、抽取音频 | Windows 用 `tools/fetch_deps.ps1` 拉取；或自行提供 |
| libcurl | HTTP 下载、站点解析 | Windows 已随项目打包在 `third_party/curl`；其它平台用系统包 |
| cpp-httplib（头文件） | `music_player` 的本地 Web 服务 | 已在 `third_party/httplib` |
| C++17 编译器 + CMake 3.16+ | 构建 | MinGW-w64（Windows）/ gcc（Linux）/ clang |

> 如果只想编译命令行、不想编译 Web 播放器，可加 `-DBUILD_PLAYER=OFF`。

### 3.2 Windows（MinGW-w64）

```powershell
# 1) 准备 FFmpeg（仅在 third_party/ffmpeg 不存在时需要，已存在会跳过）
powershell -File tools/fetch_deps.ps1

# 2) 配置并构建
cmake -G "MinGW Makefiles" -S . -B build
cmake --build build
```

构建完成后，可执行文件位于 `build/`：

- `build/audio_extractor.exe` —— 命令行提取工具
- `build/music_player.exe` —— Web 播放器
- 同时会把 FFmpeg / libcurl 的运行时 DLL、`web/`、`extension/` 一并拷贝到 `build/` 旁边，方便整体分发。

### 3.3 Linux / macOS

```bash
# 先安装依赖（示例为 Debian/Ubuntu）
sudo apt install cmake g++ libcurl4-openssl-dev \
     libavformat-dev libavcodec-dev libavutil-dev libswresample-dev

cmake -S . -B build
cmake --build build
```

### 3.4 批量测试 / 回归脚本（可选）

`tools/` 下有一些本地验证脚本（需要 Node.js）：

```bash
node tools/test_youtube_extractor.js    # 用本地 mock 验证 YouTube 提取链路
node tools/test_douyin_extractor.js     # 用本地 mock 验证抖音提取链路
node tools/verify_categories.js         # 验证分类/列表逻辑
node tools/verify_delete.js             # 验证删除（含歌词）逻辑
node tools/verify_extension.js          # 验证插件相关接口
```

---

## 四、命令行：`audio_extractor`

把单个视频 / 直链 / 直播源提取成音频文件。

### 4.1 基本用法

```
audio_extractor [选项] <视频链接>
audio_extractor [选项] -l <urls.txt>
```

- 不指定 `-o` 时默认输出 `output.mp3`。
- 不指定 `-f` 时，格式按输出文件扩展名推断（`.wav` → WAV，其它 → MP3）。
- 输出目录会自动创建，支持嵌套路径和中文。

### 4.2 选项

| 选项 | 说明 | 默认值 |
| --- | --- | --- |
| `-o, --output FILE` | 输出文件 | `output.mp3` |
| `-f, --format FMT` | 格式：`mp3` 或 `wav` | 按扩展名推断，否则 mp3 |
| `-b, --bitrate KBPS` | MP3 码率（kbps） | `192` |
| `-r, --rate HZ` | 采样率 | `44100` |
| `-c, --channels N` | 声道数 | `2` |
| `-l, --list FILE` | 批量模式：每行一个任务（`#` 注释） | — |
| `-u, --user-agent UA` | 自定义 User-Agent | 内置 |
| `--referer URL` | 设置 Referer（部分直链 / HLS 需要） | — |
| `--cookie STR` | 设置 Cookie（登录态资源） | — |
| `-h, --help` | 显示帮助 | — |

> 含空格的路径**建议加引号**：`-o "我的音乐/晴天.mp3"`。万一忘了引号，工具也会尽量自动恢复。

### 4.3 批量文件 `urls.txt` 格式

- UTF-8 编码（不要带 BOM）。
- 每行一个任务；`#` 开头为注释，空行忽略。
- 语法：`<URL> [输出文件名]`。URL 是「第一个空白符之前」的内容，后面的全部当作输出文件名——所以文件名里可以直接带空格，**无需引号**。
- 省略输出文件名时，用 `-o` 的基名自动编号：`output_1.mp3`、`output_2.mp3` …。

```
# 批量下载样板（UTF-8）
https://www.bilibili.com/video/BVxxxx   收藏/晴天 - live版.mp3
https://youtu.be/dQw4w9WgXcQ
https://example.com/live/playlist.m3u8  直播录像.wav
```

### 4.4 示例

```bash
# 单个直链 → MP3（默认 192k）
audio_extractor.exe "https://example.com/video.mp4" -o song.mp3

# B 站短链，320k，文件名含中文和空格
audio_extractor.exe "https://b23.tv/AbCdEf" -o "音乐/好听的歌 - 翻唱.mp3" -b 320

# HLS 直播源 → WAV
audio_extractor.exe "https://example.com/live/playlist.m3u8" -f wav -o rec.wav

# 带 Referer 的直链，高码率
audio_extractor.exe "https://example.com/v.m3u8" --referer "https://site/" -b 320

# 批量
audio_extractor.exe -l urls.txt -b 320
```

---

## 五、音乐播放器：`music_player`

本地启动一个 Web 服务，浏览器打开即可使用（界面语言为中文）。

### 5.1 启动参数

| 选项 | 说明 | 默认值 |
| --- | --- | --- |
| `-p, --port N` | HTTP 端口 | `8080` |
| `-d, --dir PATH` | 曲库目录（存放提取出的音频） | 系统音乐文件夹，`music` |
| `--bind ADDR` | 绑定地址；`0.0.0.0` 表示对所有网卡开放（局域网可访问） | `0.0.0.0` |
| `--local` | 仅本机可访问（等价于 `--bind 127.0.0.1`） | — |
| `--lan` | 对所有网卡开放（等价于 `--bind 0.0.0.0`） | — |
| `--no-firewall` | 不自动添加 Windows 防火墙入站规则 | 自动添加 |
| `--no-browser` | 不自动打开浏览器 | 自动打开 |
| `-h, --help` | 显示帮助 | — |

> 曲库目录和访问设置在退出时会写入 `player_config.json`（与可执行文件同目录），下次启动自动沿用。

启动后会自动打开浏览器，并显示访问地址。

### 5.2 界面分区

顶部三个视图切换：**音乐库**、**在线提取**、**歌词**。

#### 音乐库
- **列表**：显示曲目（标题 / 艺术家 / 专辑 / 时长 / 大小）。
- **搜索**：按标题、歌手、专辑过滤。
- **排序**：最近添加 / 标题 / 歌手 / 专辑 / 时长 / 大小。
- **播放全部**：把当前列表作为队列从头播放。
- **刷新**：重新扫描曲库目录（提取新歌后也可手动刷新）。
- **浏览 / 在文件夹中显示**：切换或在系统文件管理器里打开曲库目录。

#### 在线提取
- 粘贴视频 / 直播 / 直链地址，选格式与码率，「开始提取」。
- 提取在后台进行，完成后自动出现在「全部曲目」。
- 「任务队列」实时显示进度；可一键清除已完成。

#### 歌词
- 打开当前播放曲目的歌词视图，自动滚动高亮当前句。
- **自动获取**：联网检索（LRCLIB + 网易云等），命中后存为同名 `.lrc`，下次免联网。
- **选择版本**：当有多份歌词时切换。
- **时间偏移**：整体 ±0.5s 校正对齐。
- **编辑 .lrc / 移除歌词**：直接打开本地歌词文件或删除它。

#### 底部播放控制条
- 播放 / 暂停（空格）、上一首（←）、下一首（→）、随机、循环（列表循环 / 单曲循环）。
- 进度条可拖动跳转；音量可调、可静音。
- 快捷键：`空格` 播放暂停、`←/→` 切歌、`L` 打开歌词。

#### 侧边栏
- **播放列表区**：`全部曲目`、`我的收藏`，以及你自建的播放列表。
- **新建播放列表**：点列表区右上角的「+」按钮，列表保存在服务端，其它设备也能看到。
- **按专辑 / 按歌手**：根据文件名「歌名 - 歌手」自动归类；无 ID3 标签也没关系。
- **统计**：曲目数、总时长、总大小。
- **局域网**：查看本机在局域网内的访问地址。
- **浏览器插件**：打开安装引导。
- **主题**：深色 / 浅色切换。

#### 右键菜单（曲目上右键）
- 立即播放 / 下一首播放
- 添加到列表…（勾选已有列表或新建）
- 收藏 / 取消收藏
- 下载到本地 / 在文件夹中显示
- 联网获取歌词
- **删除…**（见下）

#### 删除曲目
- 右键「删除…」会弹确认框，**音频文件与同名 `.lrc` 歌词会一并永久删除**。
- 删除后自动刷新全部曲目、收藏、播放列表与统计；刷新后不再出现。

---

## 六、局域网访问

`music_player` 默认绑定 `0.0.0.0`，即对本机所有网卡开放。同一 Wi-Fi / 网线下的手机、平板、其它电脑，在界面左下角「局域网」按钮里能看到形如 `http://192.168.x.x:8080/` 的地址，直接在浏览器打开即可使用。

- 首次在 Windows 上以默认设置启动，程序会尝试自动添加一条防火墙入站规则（需要以管理员权限运行才能成功；否则会有提示，需手动放行该端口）。
- 只想自己本机用：`music_player --local`。

---

## 七、浏览器插件（抓歌到曲库）

插件位于仓库的 `extension/` 目录，是一个 MV3 扩展（Chrome / Edge 可用）。

### 安装
1. 在 Chrome / Edge 地址栏输入 `chrome://extensions`（Edge 为 `edge://extensions`）并回车。
2. 打开右上角（Edge 在左侧）的**开发者模式**。
3. 点击**加载已解压的扩展程序**，选择本项目 `extension/` 文件夹。
4. 在播放器界面点左下角「浏览器插件」，可复制 `extension` 所在路径并查看安装引导。

### 使用
- 打开任意正在播放音乐的网页，点工具栏里的紫色音符图标。
- 插件会**自动检测**当前页面中的音源（`<audio>/<video>`、网络媒体），列在面板里。
- 选一个音源，可改歌名，点「提取到全部曲目」——它会把链接交给本地播放器下载转码，完成后出现在曲库。
- 也支持在**任意链接上右键 → 提取该链接到曲库**。
- 插件只与本机服务通信（默认 `http://127.0.0.1:8080`），不经过任何第三方。
- 面板右上「设置」可改服务地址、输出格式与码率。

---

## 八、已知限制与注意事项

- **抖音提取为 best-effort（尽力而为）**：抖音网页接口现在要求 `X-Bogus` / `_signature` 签名（需用 JavaScript 计算），本项目是纯 C++ 实现、未内置 JS 引擎，因此**真实抖音链接大概率会被拒绝（未签名）并返回清晰报错**。短链 `v.douyin.com/xxx`、明确 `aweme_id` 的链接会尝试走公开接口，但能否成功取决于抖音当时的签名策略。后续若要彻底支持，需要接入轻量 JS 引擎或实现签名方案。
- **YouTube 在某些网络下不可达**：代码走 innertube `/youtubei/v1/player`，优先使用 **TVHTML5** 客户端（返回直链音频，无需跑 JS 解 signature / n 节流）。在能正常访问 YouTube 的网络下可用；若所在网络本身无法连通 YouTube，则会失败。
- **年龄 / 地区限制、私有视频**：YouTube 返回非 OK 状态时会给出明确原因（如年龄限制、地区封锁、私有），无法提取。
- **防火墙**：Windows 上若未以管理员身份运行，「自动添加防火墙规则」会失败，此时需手动放行端口才能让局域网设备访问。
- **中文 / 空格路径**：全程支持，但请注意命令行参数该加引号时加引号。
- **端口占用**：默认 8080，如被占用用 `-p` 换一个端口。

---

## 九、常见问题（FAQ）

**Q1：启动后浏览器没自动打开？**
手动访问启动日志里的地址（本机一般为 `http://127.0.0.1:8080/`）。也可加 `--no-browser` 后自己开。

**Q2：局域网里其它设备打不开？**
- 确认手机 / 电脑与运行程序的机器在同一网络。
- 确认 Windows 防火墙已放行该端口（首次建议以管理员身份启动一次以便自动加规则；或手动在「高级安全 Windows Defender 防火墙」放行入站 TCP 端口）。
- 确认启动参数没有误用 `--local`。

**Q3：粘贴链接后一直「排队中」或失败？**
- 看「任务队列」里的错误信息。B 站 / 直链 / m3u8 通常最稳；YouTube、抖音受网络与签名策略影响，详见第八节。
- 部分资源需要 `--referer` 或 `--cookie`（命令行），或在「在线提取」中确认链接正确。

**Q4：歌词没出来？**
点歌词视图的「自动获取」；若仍无结果，可在「选择版本」里手动输入歌名 / 歌手重新搜索。命中后会保存为同名 `.lrc`。

**Q5：删错歌了？**
删除会同时删掉音频和同名 `.lrc`，且不可撤销。删除前会有确认框，请确认路径无误。

**Q6：曲库目录在哪？**
默认是系统音乐文件夹下的 `music`，可在启动时用 `-d` 指定，或在界面左侧「浏览」里切换。设置会持久化到 `player_config.json`。

---

## 十、目录结构（简要）

```
audio-extractor/
├── src/
│   ├── main.cpp                # 命令行入口 (audio_extractor)
│   ├── player_main.cpp         # 播放器入口 (music_player)
│   ├── extract/                # 各站点提取器
│   │   ├── bilibili_extractor.*  # B 站
│   │   ├── youtube_extractor.*   # YouTube (innertube)
│   │   ├── douyin_extractor.*    # 抖音 (best-effort)
│   │   ├── hls_extractor.*       # m3u8
│   │   ├── direct_extractor.*    # 直链
│   │   └── registry.h            # 提取器注册/选择
│   ├── net/http_client.*       # libcurl 封装（含 POST）
│   ├── audio/audio_extractor.* # 转码/抽取核心
│   ├── library/                # 曲库扫描 + 歌词
│   └── server/player_server.*  # Web 服务与 API
├── web/                        # 播放器前端 (HTML/CSS/JS)
├── extension/                  # 浏览器插件 (MV3)
├── tools/                      # 依赖脚本与验证脚本
├── third_party/                # FFmpeg / curl / cpp-httplib
├── CMakeLists.txt
└── urls.txt                    # 批量下载样板
```
