#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
// cpp-httplib targets Windows 10+ (CreateFile2, WinSock2 helpers). MinGW
// defaults to a much older SDK, so request it explicitly.
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#elif _WIN32_WINNT < 0x0A00
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#endif

#include "player_server.h"
#include "library/music_library.h"
#include "library/lyrics.h"
#include "common/common.h"
#include "common/platform.h"
#include "common/json.h"
#include "extract/extractor.h"
#include "extract/registry.h"
#include "audio/audio_extractor.h"
#include "net/http_client.h"

#include <httplib.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <thread>
#include <tuple>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#else
#include <unistd.h>
#endif

namespace {

// ---------------------------------------------------------------------------
// Task bookkeeping
// ---------------------------------------------------------------------------
enum class TaskStatus { Queued, Running, Done, Failed };

const char* statusName(TaskStatus s) {
    switch (s) {
        case TaskStatus::Queued:  return "queued";
        case TaskStatus::Running: return "running";
        case TaskStatus::Done:    return "done";
        default:                  return "failed";
    }
}

struct Task {
    int         id = 0;
    std::string url;
    std::string output;
    std::string title;
    std::string format = "mp3";
    int         bitrate = 320;
    TaskStatus  status = TaskStatus::Queued;
    double      progress = 0;     // 0..1
    std::string stage;            // "downloading" | "transcoding" | "done" | "error"
    std::string error;
    int64_t     startedAt = 0;
    int64_t     finishedAt = 0;
};

std::string taskToJson(const Task& t) {
    return std::string("{")
        + "\"id\":"        + std::to_string(t.id)
        + ",\"url\":\""    + jsonEscape(t.url)    + "\""
        + ",\"output\":\"" + jsonEscape(t.output) + "\""
        + ",\"title\":\""  + jsonEscape(t.title)  + "\""
        + ",\"format\":\"" + jsonEscape(t.format) + "\""
        + ",\"bitrate\":"  + std::to_string(t.bitrate)
        + ",\"status\":\"" + statusName(t.status) + "\""
        + ",\"progress\":" + std::to_string(t.progress)
        + ",\"stage\":\""  + jsonEscape(t.stage)  + "\""
        + ",\"error\":\""  + jsonEscape(t.error)  + "\""
        + ",\"startedAt\":"  + std::to_string(t.startedAt)
        + ",\"finishedAt\":" + std::to_string(t.finishedAt)
        + "}";
}

std::mutex              g_taskMu;
std::vector<Task>       g_tasks;
std::atomic<int>        g_nextId{1};

int64_t nowSec() {
    return (int64_t)std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

// ---------------------------------------------------------------------------
// MIME helpers
// ---------------------------------------------------------------------------
std::string audioMime(const std::string& path) {
    std::string ext = lowerExtension(path);
    if (ext == "mp3")  return "audio/mpeg";
    if (ext == "wav")  return "audio/wav";
    if (ext == "flac") return "audio/flac";
    if (ext == "m4a" || ext == "aac") return "audio/mp4";
    if (ext == "ogg" || ext == "oga" || ext == "opus") return "audio/ogg";
    if (ext == "webm") return "audio/webm";
    if (ext == "mka")  return "audio/x-matroska";
    return "application/octet-stream";
}

std::string mimeForAsset(const std::string& name) {
    if (name.size() >= 5 && name.compare(name.size() - 5, 5, ".html") == 0) return "text/html; charset=utf-8";
    if (name.size() >= 3 && name.compare(name.size() - 3, 3, ".js")   == 0) return "application/javascript; charset=utf-8";
    if (name.size() >= 4 && name.compare(name.size() - 4, 4, ".css")  == 0) return "text/css; charset=utf-8";
    if (name.size() >= 4 && name.compare(name.size() - 4, 4, ".svg")  == 0) return "image/svg+xml";
    return "application/octet-stream";
}

// Read a whole file as UTF-8 bytes (wide-aware on Windows).
bool readWholeFile(const std::string& path, std::string& out) {
    FILE* f = plat::openFileRead(path);
    if (!f) return false;
    char buf[65536];
    size_t n;
    out.clear();
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    fclose(f);
    return true;
}

void sendJson(httplib::Response& res, int status, const std::string& body) {
    res.status = status;
    res.set_content(body, "application/json; charset=utf-8");
    res.set_header("Cache-Control", "no-store");
}

std::string trimStr(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::string getString(const JsonValue& v, const char* key, const std::string& def) {
    const JsonValue* p = v.find(key);
    if (p && p->isString()) return p->strVal;
    if (p && p->isNumber()) return p->numStr;
    return def;
}

int getInt(const JsonValue& v, const char* key, int def) {
    const JsonValue* p = v.find(key);
    if (p && p->isNumber()) return (int)p->asLong();
    return def;
}

// Pick a free destination path inside `dir`.
std::string uniqueTarget(const std::string& dir, const std::string& baseName,
                         const std::string& ext) {
    std::filesystem::path d = plat::u8path(dir);
    std::error_code ec;
    std::filesystem::create_directories(d, ec);
    std::string name = MusicLibrary::sanitizeFileName(baseName);
    std::filesystem::path cand = d / plat::u8path(name + "." + ext);
    std::string out = plat::pathToUtf8(cand);
    if (out.empty()) out = plat::pathToUtf8(d / plat::u8path("track." + ext));
    return out;
}

void setTaskProgress(int id, const std::string& stage, double p) {
    std::lock_guard<std::mutex> lk(g_taskMu);
    for (auto& t : g_tasks) {
        if (t.id == id) { t.stage = stage; t.progress = p; return; }
    }
}

void finishTask(int id, bool ok, const std::string& err) {
    std::lock_guard<std::mutex> lk(g_taskMu);
    for (auto& t : g_tasks) {
        if (t.id != id) continue;
        t.status = ok ? TaskStatus::Done : TaskStatus::Failed;
        t.stage = ok ? "done" : "error";
        t.error = err;
        t.progress = ok ? 1.0 : t.progress;
        t.finishedAt = nowSec();
        return;
    }
}

// The background work for one download+transcode request.
void runExtractTask(int id, std::string url, std::string wantName,
                    std::string fmtStr, int bitrate, const std::string& libraryDir,
                    MusicLibrary* library) {
    setTaskProgress(id, "starting", 0.01);
    try {
        IExtractor* ex = ExtractorRegistry::select(url);
        if (!ex) throw std::runtime_error("没有可用的提取器支持该链接");

        ExtractOptions opt;
        std::atomic<int> phase{0};   // 0 = downloading, 1 = transcoding
        opt.progress = [&](double frac) {
            double mapped = (frac < 0)
                ? -1
                : (phase.load() == 0 ? frac * 0.65 : 0.65 + frac * 0.35);
            if (mapped < 0) mapped = (phase.load() == 0 ? 0.02 : 0.66);
            setTaskProgress(id, phase.load() == 0 ? "downloading" : "transcoding", mapped);
        };

        std::string ext = (fmtStr == "wav") ? "wav" : "mp3";
        TempDir temp("mp_tmp");

        LOG_INFO("[task " + std::to_string(id) + "] extracting " + url);
        MediaSource src = ex->extract(url, opt, temp);

        // Decide the destination file name.
        std::string base = wantName;
        if (base.empty() && !src.suggestedName.empty()) base = src.suggestedName;
        if (base.empty()) {
            char ts[32];
            std::time_t t = std::time(nullptr);
            std::strftime(ts, sizeof(ts), "%Y%m%d-%H%M%S", std::localtime(&t));
            base = std::string("extract-") + ts;
        }
        std::string outPath = uniqueTarget(libraryDir, base, ext);

        {
            std::lock_guard<std::mutex> lk(g_taskMu);
            for (auto& t : g_tasks) {
                if (t.id == id) {
                    if (t.title.empty()) {
                        std::string fname = plat::pathToUtf8(plat::u8path(outPath).stem());
                        t.title = fname;
                    }
                    t.output = plat::pathToUtf8(plat::u8path(outPath).filename());
                    break;
                }
            }
        }

        phase.store(1);
        AudioOptions aopt;
        aopt.format = (fmtStr == "wav") ? AudioFormat::WAV : AudioFormat::MP3;
        aopt.sampleRate = 44100;
        aopt.channels = 2;
        aopt.bitrateKbps = bitrate;
        aopt.outputPath = outPath;
        aopt.progress = [&](double frac) {
            double mapped = (frac < 0) ? -1 : 0.65 + frac * 0.35;
            if (mapped < 0) mapped = 0.7;
            setTaskProgress(id, "transcoding", mapped);
        };

        int rc = extractAudio(src, aopt);
        if (rc != 0)
            throw std::runtime_error("音频转码失败 (code " + std::to_string(rc) + ")");

        finishTask(id, true, "");
        LOG_INFO("[task " + std::to_string(id) + "] done -> " + outPath);
        if (library) library->scan(true);
    } catch (const std::exception& e) {
        LOG_ERROR("[task " + std::to_string(id) + "] failed: " + e.what());
        finishTask(id, false, e.what());
    }
}

std::string defaultLibraryDir() {
    // Prefer the OS music folder; fall back to a folder beside the cwd.
#ifdef _WIN32
    const char* home = std::getenv("USERPROFILE");
#else
    const char* home = std::getenv("HOME");
#endif
    if (home) {
        std::filesystem::path mus = plat::u8path(std::string(home)) / plat::u8path("Music");
        std::error_code ec;
        if (std::filesystem::is_directory(mus, ec))
            return plat::pathToUtf8(mus);
    }
    return plat::pathToUtf8(std::filesystem::absolute(plat::u8path("music")));
}

// Reveal a path in the OS file manager (select the file if it is one).
bool revealPath(const std::string& utf8Path) {
    std::error_code ec;
    auto fs = plat::u8path(utf8Path);
    if (!std::filesystem::exists(fs, ec)) return false;
#ifdef _WIN32
    std::wstring w = plat::utf8ToWide(utf8Path);
    // explorer /select,"<path>" highlights the item; for folders it opens them.
    std::wstring args = L"/select,\"" + w + L"\"";
    if (std::filesystem::is_directory(fs, ec)) args = w;
    HINSTANCE rc = ShellExecuteW(nullptr, L"open", L"explorer", args.c_str(),
                                 nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(rc) > 32;
#elif defined(__APPLE__)
    std::string cmd = "open -R \"" + utf8Path + "\" >/dev/null 2>&1 &";
    return std::system(cmd.c_str()) == 0;
#else
    std::string target = utf8Path;
    if (!std::filesystem::is_directory(fs, ec))
        target = plat::pathToUtf8(fs.parent_path());
    std::string cmd = "xdg-open \"" + target + "\" >/dev/null 2>&1 &";
    return std::system(cmd.c_str()) == 0;
#endif
}

// RFC 5987 Content-Disposition that survives non-ASCII (Chinese) filenames.
std::string contentDispositionAttachment(const std::string& fileName) {
    static const char* hex = "0123456789ABCDEF";
    std::string ascii, encoded;
    for (unsigned char c : fileName) {
        if (c >= 0x20 && c < 0x7f && c != '"' && c != '\\') ascii += (char)c;
        else ascii += '_';
        encoded += '%';
        encoded += hex[(c >> 4) & 0xF];
        encoded += hex[c & 0xF];
    }
    if (ascii.empty()) ascii = "download";
    return "attachment; filename=\"" + ascii + "\"; filename*=UTF-8''" + encoded;
}

void openInBrowser(const std::string& url) {
#ifdef _WIN32
    std::wstring w = plat::utf8ToWide(url);
    ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#elif defined(__APPLE__)
    std::string cmd = "open \"" + url + "\" &";
    std::ignore = std::system(cmd.c_str());
#else
    std::string cmd = "xdg-open \"" + url + "\" >/dev/null 2>&1 &";
    std::ignore = std::system(cmd.c_str());
#endif
}

// ---------------------------------------------------------------------------
// LAN access helpers
// ---------------------------------------------------------------------------
bool isLoopbackBind(const std::string& addr) {
    return addr == "127.0.0.1" || addr == "::1" || addr == "localhost" ||
           addr.rfind("127.", 0) == 0;
}

// Every URL the player can be opened with, localhost first.
PlayerNetInfo buildNetInfo(const std::string& bindAddr, int port) {
    PlayerNetInfo info;
    info.bindAddr = bindAddr;
    info.port = port;
    info.hostName = plat::hostName();
    info.lanEnabled = !isLoopbackBind(bindAddr);

    const std::string suffix = ":" + std::to_string(port) + "/";
    const bool anyAddr = bindAddr.empty() || bindAddr == "0.0.0.0" || bindAddr == "::";
    info.urls.push_back("http://127.0.0.1" + suffix);
    if (info.lanEnabled) {
        if (anyAddr) {
            for (const std::string& ip : plat::lanIPv4s())
                info.urls.push_back("http://" + ip + suffix);
        } else if (bindAddr != "localhost") {
            info.urls.push_back("http://" + bindAddr + suffix);
        }
    }
    info.firewall = "skipped";
    return info;
}

#ifdef _WIN32
struct CmdResult {
    bool          started = false;
    unsigned long exitCode = 1;
    std::string   output;
};

// Run a command line hidden (no console flash) and capture its output.
CmdResult runHidden(const std::wstring& commandLine) {
    CmdResult r;
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE read = nullptr, write = nullptr;
    if (!CreatePipe(&read, &write, &sa, 0)) return r;
    SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = write;
    si.hStdError  = write;

    PROCESS_INFORMATION pi{};
    std::wstring cmd = commandLine;             // CreateProcessW may modify it
    r.started = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                               CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi) != 0;
    CloseHandle(write);
    if (!r.started) { CloseHandle(read); return r; }

    WaitForSingleObject(pi.hProcess, 30000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    r.exitCode = static_cast<unsigned long>(code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    char buf[4096];
    DWORD got = 0;
    while (ReadFile(read, buf, sizeof(buf), &got, nullptr) && got > 0)
        r.output.append(buf, got);
    CloseHandle(read);
    return r;
}

// True when the process runs with an administrator (elevated) token.
bool isProcessElevated() {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
    TOKEN_ELEVATION elev{};
    DWORD len = 0;
    const bool ok = GetTokenInformation(tok, TokenElevation, &elev, sizeof(elev), &len) != 0;
    CloseHandle(tok);
    return ok && elev.TokenIsElevated != 0;
}

std::wstring netshPath() {
    const std::wstring raw = L"%SystemRoot%\\System32\\netsh.exe";
    DWORD n = ExpandEnvironmentStringsW(raw.c_str(), nullptr, 0);
    if (n == 0) return L"netsh.exe";
    std::wstring out(static_cast<size_t>(n), L'\0');
    ExpandEnvironmentStringsW(raw.c_str(), out.data(), n);
    out = std::wstring(out.c_str());
    std::error_code ec;
    if (std::filesystem::exists(plat::u8path(plat::wideToUtf8(out)), ec)) return out;
    return L"netsh.exe";
}
#endif  // _WIN32

// Allow inbound TCP connections so devices on the same network can reach the
// player. Best effort: a failure only downgrades the report we print.
void ensureFirewallRule(const PlayerConfig& cfg, PlayerNetInfo& info) {
    info.firewallHint.clear();
    if (isLoopbackBind(cfg.bindAddr)) { info.firewall = "skipped"; return; }
#ifndef _WIN32
    info.firewall = "not-supported";
    info.firewallHint = "非 Windows 系统，请自行在防火墙中放行 TCP 端口 " +
                        std::to_string(cfg.port);
#else
    const std::wstring rule = plat::utf8ToWide(
        std::string("\"Audio Extractor Player ") + std::to_string(cfg.port) + "\"");
    const std::wstring netsh = netshPath();

    // Drop a stale rule first so the port always matches the current run.
    runHidden(netsh + L" advfirewall firewall delete rule name=" + rule);

    const std::wstring add = netsh +
        L" advfirewall firewall add rule name=" + rule +
        L" dir=in action=allow protocol=TCP localport=" +
        plat::utf8ToWide(std::to_string(cfg.port)) + L" profile=any";
    CmdResult r = runHidden(add);
    if (r.started && r.exitCode == 0) { info.firewall = "ok"; return; }
    if (!r.started) {
        info.firewall = "not-supported";
        info.firewallHint = "未找到 netsh，无法自动放行端口，请手动添加防火墙入站规则";
        return;
    }
    // The language-independent way to tell why this failed: netsh prints its
    // reason in the OS language (GBK on a Chinese Windows), so judge by
    // elevation instead of parsing the message.
    if (!isProcessElevated()) {
        info.firewall = "needs-admin";
        info.firewallHint = "当前未以管理员身份运行，未自动放行端口。若其它设备打不开，"
                            "请右键 music_player.exe 选择「以管理员身份运行」一次";
    } else {
        info.firewall = "failed";
        info.firewallHint = "防火墙放行失败，请检查 Windows Firewall 服务是否已启用";
    }
#endif
}

}  // namespace

// ---------------------------------------------------------------------------
// Config persistence
// ---------------------------------------------------------------------------
void playerLoadConfig(const std::string& path, PlayerConfig& cfg) {
    std::string text;
    if (!readWholeFile(path, text) || text.empty()) {
        cfg.libraryDir = defaultLibraryDir();
        return;
    }
    try {
        JsonValue v = JsonParser::parse(text);
        std::string dir = getString(v, "libraryDir", cfg.libraryDir);
        if (dir.empty()) dir = defaultLibraryDir();
        cfg.libraryDir = dir;
        int p = getInt(v, "port", cfg.port);
        if (p > 0 && p < 65536) cfg.port = p;
        std::string bind = getString(v, "bindAddr", cfg.bindAddr);
        if (!bind.empty()) cfg.bindAddr = bind;
        if (const JsonValue* fw = v.find("manageFirewall")) {
            if (fw->isBool()) cfg.manageFirewall = fw->boolVal;
        }
        cfg.favorites.clear();
        if (const JsonValue* fav = v.find("favorites")) {
            if (fav->isArray()) {
                for (const JsonValue& item : fav->arr)
                    cfg.favorites.push_back(item.asString());
            }
        }
        cfg.playlists.clear();
        if (const JsonValue* pls = v.find("playlists")) {
            if (pls->isArray()) {
                for (const JsonValue& p : pls->arr) {
                    if (!p.isObject()) continue;
                    Playlist pl;
                    const JsonValue* id = p.find("id");
                    const JsonValue* nm = p.find("name");
                    if (!id || !nm) continue;
                    pl.id = id->asString();
                    pl.name = nm->asString();
                    if (pl.id.empty() || pl.name.empty()) continue;
                    if (const JsonValue* ids = p.find("ids")) {
                        if (ids->isArray())
                            for (const JsonValue& t : ids->arr) pl.trackIds.push_back(t.asString());
                    }
                    cfg.playlists.push_back(std::move(pl));
                }
            }
        }
    } catch (...) {
        cfg.libraryDir = defaultLibraryDir();
    }
}

void playerSaveConfig(const std::string& path, const PlayerConfig& cfg) {
    std::filesystem::path p = plat::u8path(path);
    std::error_code ec;
    auto parent = p.parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, ec);
    std::string body = std::string("{\n  \"libraryDir\": \"") + jsonEscape(cfg.libraryDir)
                     + "\",\n  \"port\": " + std::to_string(cfg.port)
                     + ",\n  \"bindAddr\": \"" + jsonEscape(cfg.bindAddr) + "\""
                     + ",\n  \"manageFirewall\": " + (cfg.manageFirewall ? "true" : "false")
                     + ",\n  \"favorites\": [";
    for (size_t i = 0; i < cfg.favorites.size(); ++i) {
        if (i) body += ", ";
        body += "\"" + jsonEscape(cfg.favorites[i]) + "\"";
    }
    body += "],\n  \"playlists\": [";
    for (size_t i = 0; i < cfg.playlists.size(); ++i) {
        if (i) body += ",";
        body += "\n    {\"id\":\"" + jsonEscape(cfg.playlists[i].id) + "\""
             + ",\"name\":\"" + jsonEscape(cfg.playlists[i].name) + "\",\"ids\":[";
        for (size_t k = 0; k < cfg.playlists[i].trackIds.size(); ++k) {
            if (k) body += ",";
            body += "\"" + jsonEscape(cfg.playlists[i].trackIds[k]) + "\"";
        }
        body += "]}";
    }
    body += "\n  ]\n}\n";
    FILE* f = plat::openFileWrite(path);
    if (!f) return;
    fwrite(body.data(), 1, body.size(), f);
    fclose(f);
}

// ---------------------------------------------------------------------------
// Server
// ---------------------------------------------------------------------------
int runPlayerServer(PlayerConfig cfg) {
    MusicLibrary library;
    {
        std::error_code ec;
        std::filesystem::create_directories(plat::u8path(cfg.libraryDir), ec);
    }
    std::vector<std::string> dirs{cfg.libraryDir};
    library.setDirectories(dirs);

    // Starred tracks ("my favourites"), shared by every device that opens the
    // player and persisted into the config file.
    std::mutex favMu;
    std::set<std::string> favs(cfg.favorites.begin(), cfg.favorites.end());
    auto persistFavs = [&]() {
        std::lock_guard<std::mutex> lk(favMu);
        cfg.favorites.assign(favs.begin(), favs.end());
        playerSaveConfig(cfg.configPath, cfg);
    };

    // Id counter for new playlists, seeded past whatever the config holds.
    std::mutex plMu;
    long long plSeq = 0;
    for (const Playlist& p : cfg.playlists) {
        if (p.id.size() > 2) {
            const long long n = std::strtoll(p.id.c_str() + 2, nullptr, 10);
            if (n > plSeq) plSeq = n;
        }
    }

    // Remember what we are serving so the next launch (double-clicking the
    // exe without arguments) picks up the same folder.
    playerSaveConfig(cfg.configPath, cfg);

    httplib::Server svr;
    svr.set_default_headers({{"Server", "audio-extractor-player"}});

    // ---- CORS for the browser extension ------------------------------------
    // The packaged extension (chrome-extension://…) and any page served from
    // this machine (loopback / a local NIC) may call the API cross-origin;
    // random internet sites may not. Origins without an Origin header
    // (curl, the bundled UI itself) are unaffected.
    const std::vector<std::string> localIps = plat::lanIPv4s();
    svr.set_pre_routing_handler(
        [&localIps](const httplib::Request& req, httplib::Response& res)
            -> httplib::Server::HandlerResponse {
            const std::string origin = req.get_header_value("Origin");
            if (!origin.empty()) {
                const size_t sep = origin.find("://");
                const std::string scheme = sep == std::string::npos ? "" : origin.substr(0, sep);
                std::string hostport = sep == std::string::npos ? "" : origin.substr(sep + 3);
                const size_t slash = hostport.find('/');
                if (slash != std::string::npos) hostport = hostport.substr(0, slash);

                bool allowed =
                    scheme == "chrome-extension" || scheme == "moz-extension" ||
                    scheme == "safari-web-extension";
                if (!allowed && scheme == "http") {
                    std::string host = hostport;
                    const size_t colon = host.rfind(':');
                    if (colon != std::string::npos && host.find(']') == std::string::npos)
                        host = host.substr(0, colon);
                    allowed = host == "localhost" || host == "127.0.0.1" || host == "[::1]";
                    if (!allowed)
                        for (const std::string& ip : localIps)
                            if (host == ip) { allowed = true; break; }
                }
                if (allowed) {
                    res.set_header("Access-Control-Allow-Origin", origin);
                    res.set_header("Vary", "Origin");
                    res.set_header("Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS");
                    res.set_header("Access-Control-Allow-Headers", "Content-Type");
                    res.set_header("Access-Control-Max-Age", "86400");
                }
            }
            if (req.method == "OPTIONS") {
                res.status = 204;  // pre-flight: no body
                return httplib::Server::HandlerResponse::Handled;
            }
            return httplib::Server::HandlerResponse::Unhandled;
        });

    auto serveAsset = [&cfg](const std::string& rel) -> std::pair<bool, std::string> {
        std::filesystem::path root = plat::u8path(cfg.webRoot);
        std::filesystem::path full = root / plat::u8path(rel);
        std::string text;
        if (!readWholeFile(plat::pathToUtf8(full), text)) return {false, ""};
        return {true, text};
    };

    // ---- static UI ---------------------------------------------------------
    svr.Get("/", [&](const httplib::Request&, httplib::Response& res) {
        auto a = serveAsset("index.html");
        if (!a.first) { res.status = 404; res.set_content("index.html missing", "text/plain"); return; }
        res.set_content(a.second, "text/html; charset=utf-8");
        res.set_header("Cache-Control", "no-cache");
    });
    svr.Get("/index.html", [&](const httplib::Request& req, httplib::Response& res) {
        res.set_redirect("/");
    });
    svr.Get("/app.js", [&](const httplib::Request&, httplib::Response& res) {
        auto a = serveAsset("app.js");
        if (!a.first) { res.status = 404; res.set_content("", "text/plain"); return; }
        res.set_content(a.second, "application/javascript; charset=utf-8");
        res.set_header("Cache-Control", "no-cache");
    });
    svr.Get("/style.css", [&](const httplib::Request&, httplib::Response& res) {
        auto a = serveAsset("style.css");
        if (!a.first) { res.status = 404; res.set_content("", "text/plain"); return; }
        res.set_content(a.second, "text/css; charset=utf-8");
        res.set_header("Cache-Control", "no-cache");
    });
    svr.Get("/favicon.svg", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(
            "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 32 32'>"
            "<rect width='32' height='32' rx='8' fill='%237c5cff'/>"
            "<path d='M13 9v10a3 3 0 1 1-2-2.8V12l9-2v8a3 3 0 1 1-2-2.8V11l-5 1z' fill='white'/>"
            "</svg>", "image/svg+xml");
    });

    // ---- health ------------------------------------------------------------
    svr.Get("/api/health", [](const httplib::Request&, httplib::Response& res) {
        sendJson(res, 200, "{\"ok\":true}");
    });

    // ---- config ------------------------------------------------------------
    svr.Get("/api/config", [&](const httplib::Request&, httplib::Response& res) {
        sendJson(res, 200,
            std::string("{\"libraryDir\":\"") + jsonEscape(cfg.libraryDir) + "\""
            + ",\"port\":" + std::to_string(cfg.port)
            + ",\"formats\":[\"mp3\",\"wav\"]"
            + ",\"defaultBitrate\":320}");
    });

    svr.Post("/api/config", [&](const httplib::Request& req, httplib::Response& res) {
        try {
            JsonValue v = JsonParser::parse(req.body);
            std::string dir = getString(v, "libraryDir", "");
            if (dir.empty()) { sendJson(res, 400, "{\"error\":\"libraryDir 不能为空\"}"); return; }
            std::error_code ec;
            std::filesystem::create_directories(plat::u8path(dir), ec);
            if (!std::filesystem::is_directory(plat::u8path(dir), ec)) {
                sendJson(res, 400, "{\"error\":\"目录不可用: " + jsonEscape(dir) + "\"}");
                return;
            }
            cfg.libraryDir = dir;
            library.setDirectories(std::vector<std::string>{dir});
            library.scan(true);
            playerSaveConfig(cfg.configPath, cfg);
            sendJson(res, 200, std::string("{\"ok\":true,\"libraryDir\":\"") + jsonEscape(dir) + "\"}");
        } catch (const std::exception& e) {
            sendJson(res, 400, std::string("{\"error\":\"") + jsonEscape(e.what()) + "\"}");
        }
    });

    // ---- directory browser (for the in-page folder picker) -----------------
    svr.Get("/api/browse", [&](const httplib::Request& req, httplib::Response& res) {
        std::string dir = req.get_param_value("path");
        std::string body = "{\"drives\":[";
#ifdef _WIN32
        DWORD mask = GetLogicalDrives();
        bool firstDrive = true;
        for (int i = 0; i < 26; ++i) {
            if (!(mask & (1u << i))) continue;
            std::string letter = std::string(1, (char)('A' + i)) + ":\\";
            if (!firstDrive) body += ",";
            firstDrive = false;
            body += "{\"name\":\"" + jsonEscape(letter) + "\",\"path\":\"" + jsonEscape(letter) + "\"}";
        }
#endif
        body += "],\"dirs\":[";

        if (!dir.empty()) {
            std::error_code ec;
            auto root = plat::u8path(dir);
            if (std::filesystem::is_directory(root, ec)) {
                bool first = true;
                std::vector<std::pair<std::string, std::string>> entries;  // name, path
                for (auto& de : std::filesystem::directory_iterator(
                         root, std::filesystem::directory_options::skip_permission_denied, ec)) {
                    if (de.is_directory(ec)) {
                        entries.emplace_back(plat::pathToUtf8(de.path().filename()),
                                             plat::pathToUtf8(de.path()));
                    }
                }
                // Case-insensitive, natural-ish ordering for Chinese + Latin.
                std::sort(entries.begin(), entries.end(),
                          [](const auto& a, const auto& b) { return a.first < b.first; });
                for (auto& e : entries) {
                    if (!first) body += ",";
                    first = false;
                    body += "{\"name\":\"" + jsonEscape(e.first) +
                            "\",\"path\":\"" + jsonEscape(e.second) + "\"}";
                }
                body += "],\"path\":\"" + jsonEscape(dir) + "\"";
                auto parent = root.parent_path();
                std::string parentStr = parent.empty() ? "" : plat::pathToUtf8(parent);
                // "C:\\" has an empty parent; stay put rather than jumping to "".
                if (parentStr.size() && parentStr.back() == ':') parentStr = "";
                body += ",\"parent\":\"" + jsonEscape(parentStr) + "\"}";
                sendJson(res, 200, body);
                return;
            }
        }
        body += "],\"path\":\"\",\"parent\":\"\"}";
        sendJson(res, 200, body);
    });

    // ---- reveal a track / open its folder ----------------------------------
    svr.Post("/api/reveal", [&](const httplib::Request& req, httplib::Response& res) {
        try {
            std::string target;
            if (!req.body.empty() && req.body[0] == '{') {
                JsonValue v = JsonParser::parse(req.body);
                std::string id = getString(v, "id", "");
                if (!id.empty()) {
                    TrackMeta t;
                    if (library.find(id, t)) target = t.path;
                }
                if (target.empty()) target = getString(v, "path", "");
            }
            if (target.empty()) { sendJson(res, 400, "{\"error\":\"缺少 id 或 path\"}"); return; }
            if (!revealPath(target)) { sendJson(res, 500, "{\"error\":\"无法在系统文件管理器中打开\"}"); return; }
            sendJson(res, 200, "{\"ok\":true}");
        } catch (const std::exception& e) {
            sendJson(res, 400, std::string("{\"error\":\"") + jsonEscape(e.what()) + "\"}");
        }
    });

    // ---- library -----------------------------------------------------------
    svr.Get("/api/library", [&](const httplib::Request& req, httplib::Response& res) {
        bool force = req.has_param("refresh") &&
                     req.get_param_value("refresh") != "0";
        auto tracks = library.scan(force);
        std::string body = "{\"dirs\":[\"" + jsonEscape(cfg.libraryDir) + "\"],\"tracks\":[";
        for (size_t i = 0; i < tracks.size(); ++i) {
            if (i) body += ",";
            // trackToJson ends with '}': reopen it to add "favorite".
            std::string tj = trackToJson(tracks[i]);
            tj.pop_back();
            {
                std::lock_guard<std::mutex> lk(favMu);
                tj += ",\"favorite\":" + std::string(favs.count(tracks[i].id) ? "true" : "false");
            }
            tj += "}";
            body += tj;
        }
        body += "],\"count\":" + std::to_string(tracks.size()) + "}";
        sendJson(res, 200, body);
    });

    // ---- favourites -------------------------------------------------------
    svr.Get("/api/favorites", [&](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lk(favMu);
        std::string body = "{\"ids\":[";
        size_t n = 0;
        for (const std::string& id : favs) {
            if (n++) body += ",";
            body += "\"" + jsonEscape(id) + "\"";
        }
        body += "]}";
        sendJson(res, 200, body);
    });

    svr.Post("/api/favorites", [&](const httplib::Request& req, httplib::Response& res) {
        try {
            JsonValue v = JsonParser::parse(req.body);
            std::string id = trimStr(getString(v, "id", ""));
            if (id.empty()) { sendJson(res, 400, "{\"error\":\"缺少曲目 id\"}"); return; }
            bool on = true;
            if (const JsonValue* f = v.find("on")) {
                if (f->isBool()) on = f->boolVal;
            }
            {
                std::lock_guard<std::mutex> lk(favMu);
                if (on) favs.insert(id); else favs.erase(id);
            }
            persistFavs();
            sendJson(res, 200, std::string("{\"ok\":true,\"favorite\":")
                     + (on ? "true" : "false") + "}");
        } catch (const std::exception& e) {
            sendJson(res, 400, std::string("{\"error\":\"") + jsonEscape(e.what()) + "\"}");
        }
    });

    // ---- user playlists ----------------------------------------------------
    // Playlists live alongside the favourites in the config file, so every
    // device that opens the player sees the same lists.
    auto playlistJson = [](const Playlist& p) {
        std::string s = "{\"id\":\"" + jsonEscape(p.id) + "\",\"name\":\"" + jsonEscape(p.name)
                      + "\",\"count\":" + std::to_string(p.trackIds.size()) + ",\"ids\":[";
        for (size_t i = 0; i < p.trackIds.size(); ++i) {
            if (i) s += ",";
            s += "\"" + jsonEscape(p.trackIds[i]) + "\"";
        }
        return s + "]}";
    };
    auto persist = [&]() { playerSaveConfig(cfg.configPath, cfg); };

    svr.Get("/api/playlists", [&](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lk(plMu);
        std::string body = "{\"playlists\":[";
        for (size_t i = 0; i < cfg.playlists.size(); ++i) {
            if (i) body += ",";
            body += playlistJson(cfg.playlists[i]);
        }
        body += "]}";
        sendJson(res, 200, body);
    });

    svr.Post("/api/playlists", [&](const httplib::Request& req, httplib::Response& res) {
        try {
            JsonValue v = JsonParser::parse(req.body);
            std::string name = trimStr(getString(v, "name", ""));
            if (name.empty()) { sendJson(res, 400, "{\"error\":\"列表名不能为空\"}"); return; }
            if (name.size() > 60) name = name.substr(0, 60);
            Playlist p;
            {
                std::lock_guard<std::mutex> lk(plMu);
                const bool exists = std::any_of(cfg.playlists.begin(), cfg.playlists.end(),
                                                [&](const Playlist& x) { return x.name == name; });
                if (exists) { sendJson(res, 400, "{\"error\":\"已经有同名列表了\"}"); return; }
                p.name = name;
                p.id = "pl" + std::to_string(++plSeq);
                cfg.playlists.push_back(p);
            }
            persist();
            sendJson(res, 200, std::string("{\"ok\":true,\"playlist\":") + playlistJson(p) + "}");
        } catch (const std::exception& e) {
            sendJson(res, 400, std::string("{\"error\":\"") + jsonEscape(e.what()) + "\"}");
        }
    });

    svr.Delete("/api/playlists", [&](const httplib::Request& req, httplib::Response& res) {
        std::string id;
        try {
            if (!req.body.empty()) id = trimStr(getString(JsonParser::parse(req.body), "id", ""));
        } catch (...) { /* fall back to the query string */ }
        if (id.empty()) id = req.get_param_value("id");
        bool removed = false;
        {
            std::lock_guard<std::mutex> lk(plMu);
            const size_t before = cfg.playlists.size();
            cfg.playlists.erase(
                std::remove_if(cfg.playlists.begin(), cfg.playlists.end(),
                               [&](const Playlist& x) { return x.id == id; }),
                cfg.playlists.end());
            removed = cfg.playlists.size() != before;
        }
        if (!removed) { sendJson(res, 404, "{\"error\":\"列表不存在\"}"); return; }
        persist();
        sendJson(res, 200, "{\"ok\":true}");
    });

    svr.Post("/api/playlists/tracks", [&](const httplib::Request& req, httplib::Response& res) {
        try {
            JsonValue v = JsonParser::parse(req.body);
            std::string plId   = trimStr(getString(v, "playlistId", ""));
            std::string trackId = trimStr(getString(v, "trackId", ""));
            if (plId.empty() || trackId.empty()) {
                sendJson(res, 400, "{\"error\":\"缺少列表或曲目 id\"}"); return;
            }
            bool added = false;
            int  count = 0;
            {
                std::lock_guard<std::mutex> lk(plMu);
                Playlist* p = nullptr;
                for (Playlist& x : cfg.playlists) if (x.id == plId) { p = &x; break; }
                if (!p) { sendJson(res, 404, "{\"error\":\"列表不存在\"}"); return; }
                auto& ids = p->trackIds;
                auto it = std::find(ids.begin(), ids.end(), trackId);
                if (it == ids.end()) { ids.push_back(trackId); added = true; }
                else ids.erase(it);   // toggling the same entry removes it again
                count = static_cast<int>(ids.size());
            }
            persist();
            sendJson(res, 200, std::string("{\"ok\":true,\"added\":")
                     + (added ? "true" : "false") + ",\"count\":" + std::to_string(count) + "}");
        } catch (const std::exception& e) {
            sendJson(res, 400, std::string("{\"error\":\"") + jsonEscape(e.what()) + "\"}");
        }
    });

    // ---- cover art ---------------------------------------------------------
    svr.Get("/api/cover", [&](const httplib::Request& req, httplib::Response& res) {
        std::string id = req.get_param_value("id");
        std::vector<uint8_t> bytes;
        std::string mime;
        if (id.empty() || !library.cover(id, bytes, mime)) {
            res.status = 404;
            res.set_content("", "image/png");
            return;
        }
        res.set_content(reinterpret_cast<const char*>(bytes.data()), bytes.size(), mime);
        res.set_header("Cache-Control", "max-age=3600");
    });

    // ---- audio streaming with HTTP Range support ---------------------------
    svr.Get("/api/stream", [&](const httplib::Request& req, httplib::Response& res) {
        std::string id = req.get_param_value("id");
        TrackMeta t;
        if (id.empty() || !library.find(id, t)) {
            sendJson(res, 404, "{\"error\":\"曲目不存在\"}");
            return;
        }
        std::error_code ec;
        auto sz = std::filesystem::file_size(plat::u8path(t.path), ec);
        if (ec || sz == 0) {
            // Fall back to the cached size if the file vanished from disk.
            sz = (uintmax_t)std::max<int64_t>(t.size, 0);
            if (sz == 0) { sendJson(res, 404, "{\"error\":\"文件不可读\"}"); return; }
        }

        FILE* f = plat::openFileRead(t.path);
        if (!f) { sendJson(res, 404, "{\"error\":\"无法打开文件\"}"); return; }

        res.set_header("Accept-Ranges", "bytes");
        res.set_header("Cache-Control", "no-cache");
        std::string mimeType = audioMime(t.path);
        res.set_header("Content-Type", mimeType);
        if (req.has_header("Range")) res.status = httplib::StatusCode::PartialContent_206;

        // httplib clamps the requested range, emits Content-Range, sets the
        // partial Content-Length, and answers 416 on unsatisfiable ranges.
        // Our provider therefore just has to honour the absolute offset.
        res.set_content_provider(
            static_cast<size_t>(sz), mimeType,
            [f](size_t offset, size_t length, httplib::DataSink& sink) -> bool {
                if (plat::seek64(f, (int64_t)offset, SEEK_SET) != 0) return false;
                std::vector<char> buf(64 * 1024);
                size_t remaining = length;
                while (remaining > 0) {
                    size_t chunk = std::min<size_t>(remaining, buf.size());
                    size_t got = fread(buf.data(), 1, chunk, f);
                    if (got == 0) break;
                    if (!sink.write(buf.data(), got)) return false;
                    remaining -= got;
                }
                return true;
            },
            [f](bool) { fclose(f); });
    });

    // ---- download (attachment) --------------------------------------------
    svr.Get("/api/download", [&](const httplib::Request& req, httplib::Response& res) {
        std::string id = req.get_param_value("id");
        TrackMeta t;
        if (id.empty() || !library.find(id, t)) {
            sendJson(res, 404, "{\"error\":\"曲目不存在\"}");
            return;
        }
        std::string text;
        if (!readWholeFile(t.path, text)) { sendJson(res, 404, "{\"error\":\"无法读取文件\"}"); return; }
        res.set_header("Content-Disposition", contentDispositionAttachment(t.fileName));
        res.set_content(text, audioMime(t.path));
    });

    // ---- delete a track together with its lyric ----------------------------
    svr.Delete("/api/track", [&](const httplib::Request& req, httplib::Response& res) {
        const std::string id = req.get_param_value("id");
        TrackMeta t;
        if (!library.find(id, t)) {
            sendJson(res, 404, "{\"ok\":false,\"error\":\"曲目不存在\"}");
            return;
        }

        // Hard safety net: refuse anything outside the configured library so
        // a crafted id/endpoint can never delete an arbitrary file.
        std::error_code ec;
        const std::filesystem::path root =
            std::filesystem::weakly_canonical(plat::u8path(cfg.libraryDir), ec);
        const std::filesystem::path file =
            std::filesystem::weakly_canonical(plat::u8path(t.path), ec);
        bool inside = !file.empty();
        if (inside) {
            bool found = false;
            for (std::filesystem::path it = file; ; it = it.parent_path()) {
                if (it == root) { found = true; break; }
                if (it.parent_path() == it) break;   // reached "/"
            }
            inside = found;
        }
        if (!inside) {
            sendJson(res, 403, "{\"ok\":false,\"error\":\"只允许删除曲库目录内的文件\"}");
            return;
        }

        // Windows keeps the file locked until the last /api/stream reader
        // closes its handle, so give a just-stopped player a moment to let go.
        std::error_code dec;
        for (int attempt = 0; attempt < 8; ++attempt) {
            dec.clear();
            std::filesystem::remove(file, dec);
            if (!dec) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
        }
        if (dec) {
            LOG_ERROR("Delete failed for " + t.path + ": " + dec.message());
            sendJson(res, 500, "{\"ok\":false,\"error\":\"文件正在被占用（可能还在播放），"
                               "请先停止播放后重试\"}");
            return;
        }

        // The lyric file shares the audio file's stem; remove it too.
        bool lyricDeleted = false;
        const std::filesystem::path lrc = file.parent_path() / (file.stem().wstring() + L".lrc");
        std::error_code lec;
        if (std::filesystem::exists(lrc, lec)) {
            lyricDeleted = std::filesystem::remove(lrc, lec);
            if (lyricDeleted) LOG_INFO("Deleted lyric: " + plat::pathToUtf8(lrc));
        }

        // Nothing should keep referencing a file that no longer exists.
        int fromFav = 0, fromPlaylists = 0;
        {
            std::lock_guard<std::mutex> lk(favMu);
            fromFav = (int)favs.erase(t.id);
        }
        {
            std::lock_guard<std::mutex> lk(plMu);
            for (Playlist& p : cfg.playlists) {
                const size_t before = p.trackIds.size();
                p.trackIds.erase(std::remove(p.trackIds.begin(), p.trackIds.end(), t.id),
                                 p.trackIds.end());
                if (p.trackIds.size() != before) ++fromPlaylists;
            }
        }
        persistFavs();
        playerSaveConfig(cfg.configPath, cfg);
        library.scan(true);

        LOG_INFO("Deleted track: " + t.path);
        sendJson(res, 200, std::string("{\"ok\":true,\"id\":\"") + jsonEscape(t.id) + "\""
                 + ",\"title\":\"" + jsonEscape(t.title) + "\""
                 + ",\"lyricDeleted\":" + (lyricDeleted ? "true" : "false")
                 + ",\"removedFromFavorites\":" + (fromFav ? "true" : "false")
                 + ",\"removedFromPlaylists\":" + std::to_string(fromPlaylists)
                 + "}");
    });

    // ---- lyrics ------------------------------------------------------------
    // The client always talks in absolute seconds; LRC serialisation stays
    // server-side so the UI never has to deal with the text format.
    auto lyricToJson = [](const lyr::Lyric& l) {
        std::string b = "{"
            "\"found\":"     + std::string(l.found ? "true" : "false")
          + ",\"synced\":"    + std::string(l.synced ? "true" : "false")
          + ",\"source\":\""  + jsonEscape(l.source) + "\""
          + ",\"localPath\":\"" + jsonEscape(l.localPath) + "\""
          + ",\"offset\":"    + std::to_string(l.offset)
          + ",\"title\":\""   + jsonEscape(l.title) + "\""
          + ",\"artist\":\""  + jsonEscape(l.artist) + "\""
          + ",\"album\":\""   + jsonEscape(l.album) + "\""
          + ",\"lines\":[";
        for (std::size_t i = 0; i < l.lines.size(); ++i) {
            if (i) b += ",";
            char t[32];
            std::snprintf(t, sizeof(t), "%.2f", l.lines[i].time);
            b += "{\"t\":" + std::string(t)
               + ",\"text\":\"" + jsonEscape(l.lines[i].text) + "\"}";
        }
        b += "]}";
        return b;
    };

    // Resolve a track id -> media path, replying with an error JSON on failure.
    auto resolveTrack = [&](const httplib::Request& req, httplib::Response& res,
                            TrackMeta& t) -> bool {
        std::string id = req.get_param_value("id");
        if (id.empty() && req.has_param("id")) id = req.get_param_value("id");
        if (!library.find(id, t)) {
            sendJson(res, 404, "{\"ok\":false,\"error\":\"曲目不存在\"}");
            return false;
        }
        return true;
    };

    svr.Get("/api/lyric", [&](const httplib::Request& req, httplib::Response& res) {
        TrackMeta t;
        if (!resolveTrack(req, res, t)) return;

        lyr::Lyric l;
        if (lyr::loadLocal(t.path, l)) {
            // Fall back to the file's own tags for any empty metadata field.
            if (l.title.empty())  l.title  = t.title;
            if (l.artist.empty()) l.artist = t.artist;
            if (l.album.empty())  l.album  = t.album;
            sendJson(res, 200, "{\"ok\":true,\"lyric\":" + lyricToJson(l) + "}");
            return;
        }

        // Nothing beside the media file: surface whatever is embedded so the
        // UI can show *something* without ever touching the network.
        lyr::Lyric emb;
        bool useEmbedded = req.get_param_value("embed") != "0" && lyr::readEmbedded(t.path, emb);
        if (useEmbedded) {
            if (emb.title.empty())  emb.title  = t.title;
            if (emb.artist.empty()) emb.artist = t.artist;
            if (emb.album.empty())  emb.album  = t.album;
            sendJson(res, 200, "{\"ok\":true,\"lyric\":" + lyricToJson(emb) + "}");
            return;
        }

        sendJson(res, 200,
                 "{\"ok\":true,\"lyric\":{\"found\":false,\"synced\":false,"
                 "\"source\":\"\",\"localPath\":\"" + jsonEscape(lyr::lrcPathFor(t.path)) +
                 "\",\"offset\":0,\"title\":\"\",\"artist\":\"\",\"album\":\"\",\"lines\":[]}}");
    });

    svr.Post("/api/lyric/search", [&](const httplib::Request& req, httplib::Response& res) {
        TrackMeta t;
        std::string id;
        JsonValue body;
        bool parsed = false;
        try { body = JsonParser::parse(req.body); parsed = true; } catch (...) {}
        if (parsed) id = getString(body, "id", "");
        if (id.empty()) id = req.get_param_value("id");
        if (!library.find(id, t)) {
            sendJson(res, 404, "{\"ok\":false,\"error\":\"曲目不存在\"}");
            return;
        }

        lyr::Query q;
        q.title = t.title;
        q.artist = t.artist;
        q.album = t.album;
        q.duration = t.duration;

        // A manually corrected query (typed in the UI) always wins.
        if (parsed) {
            std::string ti = getString(body, "title", "");
            std::string ar = getString(body, "artist", "");
            if (!ti.empty()) q.title = ti;
            if (!ar.empty()) q.artist = ar;
        }

        std::string err;
        auto hits = lyr::search(q, 12, &err);

        std::string b = "{\"ok\":true,\"error\":\"" + jsonEscape(err) + "\""
                      + ",\"query\":{\"title\":\"" + jsonEscape(q.title)
                      + "\",\"artist\":\"" + jsonEscape(q.artist) + "\"}"
                      + ",\"hits\":[";
        for (std::size_t i = 0; i < hits.size(); ++i) {
            const auto& h = hits[i];
            if (i) b += ",";
            char sc[32];
            std::snprintf(sc, sizeof(sc), "%.1f", h.score);
            b += "{\"provider\":\""  + jsonEscape(h.provider) + "\""
               + ",\"remoteId\":\""  + jsonEscape(h.remoteId) + "\""
               + ",\"title\":\""     + jsonEscape(h.title)    + "\""
               + ",\"artist\":\""    + jsonEscape(h.artist)   + "\""
               + ",\"album\":\""     + jsonEscape(h.album)    + "\""
               + ",\"preview\":\""   + jsonEscape(h.preview)  + "\""
               + ",\"duration\":"    + std::to_string(h.duration)
               + ",\"synced\":"      + std::string(h.synced ? "true" : "false")
               + ",\"score\":"       + std::string(sc)
               + "}";
        }
        b += "]}";
        if (hits.empty() && !err.empty()) LOG_WARN("lyric search failed: " + err);
        sendJson(res, 200, b);
    });

    svr.Post("/api/lyric/save", [&](const httplib::Request& req, httplib::Response& res) {
        TrackMeta t;
        JsonValue body;
        try { body = JsonParser::parse(req.body); } catch (...) {
            sendJson(res, 400, "{\"ok\":false,\"error\":\"请求体不是合法 JSON\"}");
            return;
        }
        std::string id = getString(body, "id", "");
        if (!library.find(id, t)) {
            sendJson(res, 404, "{\"ok\":false,\"error\":\"曲目不存在\"}");
            return;
        }

        std::string lrc = getString(body, "lrc", "");
        if (lrc.empty()) {
            lyr::SearchHit h;
            h.provider  = getString(body, "provider", "");
            h.remoteId  = getString(body, "remoteId", "");
            std::string err;
            if (!lyr::fetchByHit(h, lrc, &err)) {
                sendJson(res, 502, "{\"ok\":false,\"error\":\"" + jsonEscape(err) + "\"}");
                return;
            }
        }

        lyr::Lyric parsed = lyr::parseLrc(lrc);
        if (!parsed.hasAnyText()) {
            sendJson(res, 400, "{\"ok\":false,\"error\":\"歌词为空或无法解析\"}");
            return;
        }
        // Stamp what we know so the .lrc is self-describing on disk.
        if (parsed.title.empty())  parsed.title  = t.title;
        if (parsed.artist.empty()) parsed.artist = t.artist;
        if (parsed.album.empty())  parsed.album  = t.album;

        std::string written;
        if (!lyr::saveLocal(t.path, lyr::toLrc(parsed), &written)) {
            sendJson(res, 500, "{\"ok\":false,\"error\":\"无法写入歌词文件（目录只读？）\"}");
            return;
        }
        lyr::Lyric saved;
        lyr::loadLocal(t.path, saved);
        LOG_INFO("saved lyric -> " + written);
        sendJson(res, 200, "{\"ok\":true,\"path\":\"" + jsonEscape(written) +
                           "\",\"lyric\":" + lyricToJson(saved) + "}");
    });

    // Convenience: search, take the best hit, save it. One round-trip so the
    // common "just get me the lyrics" click stays instant.
    svr.Post("/api/lyric/auto", [&](const httplib::Request& req, httplib::Response& res) {
        TrackMeta t;
        JsonValue body;
        bool hasBody = false;
        try { body = JsonParser::parse(req.body); hasBody = true; } catch (...) {}
        std::string id = hasBody ? getString(body, "id", "") : req.get_param_value("id");
        if (!library.find(id, t)) {
            sendJson(res, 404, "{\"ok\":false,\"error\":\"曲目不存在\"}");
            return;
        }

        lyr::Query q{t.title, t.artist, t.album, t.duration};
        if (hasBody) {
            std::string ti = getString(body, "title", "");
            std::string ar = getString(body, "artist", "");
            if (!ti.empty()) q.title = ti;
            if (!ar.empty()) q.artist = ar;
        }

        std::string err;
        auto hits = lyr::search(q, 1, &err);
        if (hits.empty()) {
            sendJson(res, 404, "{\"ok\":false,\"error\":\"没有找到匹配的歌词" +
                     (err.empty() ? std::string() : "（" + err + "）") + "\"}");
            return;
        }

        std::string lrc;
        if (!lyr::fetchByHit(hits[0], lrc, &err)) {
            sendJson(res, 502, "{\"ok\":false,\"error\":\"" + jsonEscape(err) + "\"}");
            return;
        }
        lyr::Lyric parsed = lyr::parseLrc(lrc);
        if (!parsed.hasAnyText()) {
            sendJson(res, 404, "{\"ok\":false,\"error\":\"匹配到的歌词为空\"}");
            return;
        }
        if (parsed.title.empty())  parsed.title  = hits[0].title;
        if (parsed.artist.empty()) parsed.artist = hits[0].artist;
        if (parsed.album.empty())  parsed.album  = hits[0].album;

        std::string written;
        if (!lyr::saveLocal(t.path, lyr::toLrc(parsed), &written)) {
            sendJson(res, 500, "{\"ok\":false,\"error\":\"无法写入歌词文件\"}");
            return;
        }
        lyr::Lyric saved;
        lyr::loadLocal(t.path, saved);
        std::string hit = "{\"title\":\"" + jsonEscape(hits[0].title)
                        + "\",\"artist\":\"" + jsonEscape(hits[0].artist)
                        + "\",\"provider\":\"" + jsonEscape(hits[0].provider) + "\"}";
        sendJson(res, 200, "{\"ok\":true,\"path\":\"" + jsonEscape(written) +
                           "\",\"hit\":" + hit +
                           ",\"lyric\":" + lyricToJson(saved) + "}");
    });

    svr.Delete("/api/lyric", [&](const httplib::Request& req, httplib::Response& res) {
        TrackMeta t;
        if (!resolveTrack(req, res, t)) return;
        bool removed = lyr::removeLocal(t.path);
        sendJson(res, 200, std::string("{\"ok\":true,\"removed\":") +
                           (removed ? "true" : "false") + "}");
    });

    // ---- extraction tasks --------------------------------------------------
    svr.Get("/api/tasks", [](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lk(g_taskMu);
        std::string body = "{\"tasks\":[";
        for (size_t i = 0; i < g_tasks.size(); ++i) {
            if (i) body += ",";
            body += taskToJson(g_tasks[i]);
        }
        body += "]}";
        sendJson(res, 200, body);
    });

    svr.Post("/api/tasks/clear", [](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lk(g_taskMu);
        std::vector<Task> kept;
        for (auto& t : g_tasks)
            if (t.status == TaskStatus::Running || t.status == TaskStatus::Queued)
                kept.push_back(t);
        g_tasks.swap(kept);
        sendJson(res, 200, "{\"ok\":true}");
    });

    // ---- network reachability (LAN sharing) --------------------------------
    PlayerNetInfo net = buildNetInfo(cfg.bindAddr, cfg.port);
    if (cfg.manageFirewall) ensureFirewallRule(cfg, net);
    else { net.firewall = "skipped"; net.firewallHint = "已通过 --no-firewall 关闭自动放行"; }

    svr.Get("/api/net", [&](const httplib::Request&, httplib::Response& res) {
        std::string body = "{\"bindAddr\":\"" + jsonEscape(net.bindAddr) + "\""
                         + ",\"port\":" + std::to_string(net.port)
                         + ",\"lanEnabled\":" + (net.lanEnabled ? "true" : "false")
                         + ",\"hostName\":\"" + jsonEscape(net.hostName) + "\""
                         + ",\"firewall\":\"" + jsonEscape(net.firewall) + "\""
                         + ",\"firewallHint\":\"" + jsonEscape(net.firewallHint) + "\""
                         + ",\"urls\":[";
        for (size_t i = 0; i < net.urls.size(); ++i)
            body += (i ? ",\"" : "\"") + jsonEscape(net.urls[i]) + "\"";
        body += "]}";
        sendJson(res, 200, body);
    });

    // ---- browser extension info --------------------------------------------
    svr.Get("/api/extension/info", [&](const httplib::Request&, httplib::Response& res) {
        // The extension folder sits next to the web root (…/build/extension).
        std::filesystem::path extDir = plat::u8path(cfg.webRoot).parent_path() / "extension";
        std::error_code ec;
        const bool available = std::filesystem::exists(extDir / "manifest.json", ec);
        std::string body = "{\"available\":" + std::string(available ? "true" : "false")
                         + ",\"dir\":\"" + jsonEscape(plat::pathToUtf8(extDir)) + "\""
                         + ",\"serverUrl\":\"http://127.0.0.1:" + std::to_string(cfg.port) + "/\""
                         + ",\"libraryDir\":\"" + jsonEscape(cfg.libraryDir) + "\""
                         + ",\"format\":\"mp3\",\"defaultBitrate\":320}";
        sendJson(res, 200, body);
    });

    svr.Post("/api/extract", [&](const httplib::Request& req, httplib::Response& res) {
        try {
            JsonValue v = JsonParser::parse(req.body);
            std::string url = getString(v, "url", "");
            url = trimStr(url);
            if (url.empty()) { sendJson(res, 400, "{\"error\":\"链接不能为空\"}"); return; }
            std::string name = getString(v, "output", "");
            std::string fmt  = getString(v, "format", "mp3");
            if (fmt != "mp3" && fmt != "wav") fmt = "mp3";
            int bitrate = getInt(v, "bitrate", 320);
            if (bitrate < 64 || bitrate > 320) bitrate = 320;

            Task t;
            t.id = g_nextId.fetch_add(1);
            t.url = url;
            t.title = name;
            t.format = fmt;
            t.bitrate = bitrate;
            t.status = TaskStatus::Queued;
            t.stage = "queued";
            t.startedAt = nowSec();
            {
                std::lock_guard<std::mutex> lk(g_taskMu);
                g_tasks.push_back(t);
            }
            std::thread worker(runExtractTask, t.id, url, name, fmt, bitrate,
                               cfg.libraryDir, &library);
            worker.detach();
            sendJson(res, 200, std::string("{\"ok\":true,\"taskId\":") + std::to_string(t.id) + "}");
        } catch (const std::exception& e) {
            sendJson(res, 400, std::string("{\"error\":\"") + jsonEscape(e.what()) + "\"}");
        }
    });

    // ---- go ----------------------------------------------------------------
    const std::string localUrl = net.urls.empty() ? "" : net.urls.front();
    LOG_INFO("Music player listening on " + cfg.bindAddr + ":" + std::to_string(cfg.port));
    LOG_INFO("Music library: " + cfg.libraryDir);

    if (cfg.openBrowser) {
        std::string open = localUrl.empty() ? "http://127.0.0.1:" + std::to_string(cfg.port) + "/" : localUrl;
        std::thread([open]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            openInBrowser(open);
        }).detach();
    }

    std::cout << "\n  本地音乐播放器已启动\n"
              << "  本机访问: " << localUrl << "\n";
    if (net.lanEnabled) {
        if (net.urls.size() > 1) {
            std::cout << "  局域网访问（同一 Wi-Fi / 网线的手机、平板、电脑）:\n";
            for (size_t i = 1; i < net.urls.size(); ++i)
                std::cout << "    " << net.urls[i] << "\n";
        } else {
            std::cout << "  未检测到局域网 IP（可能未连接网络）\n";
        }
        if (net.firewall != "ok")
            std::cout << "  防火墙: " << net.firewall
                      << (net.firewallHint.empty() ? "" : " — " + net.firewallHint) << "\n";
    } else {
        std::cout << "  仅本机可访问（--bind 设置具体地址可开放给局域网）\n";
    }
    std::cout << "  曲库目录: " << cfg.libraryDir << "\n"
              << "  (停止服务请在此窗口按 Ctrl+C)\n\n" << std::flush;

    if (!svr.listen(cfg.bindAddr, cfg.port)) {
        std::cerr << "Error: failed to bind " << cfg.bindAddr << ":" << cfg.port << "\n";
        return 1;
    }
    return 0;
}
