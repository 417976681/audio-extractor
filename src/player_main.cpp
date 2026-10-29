#include "server/player_server.h"
#include "common/common.h"
#include "common/platform.h"
#include "net/http_client.h"
#include "extract/extractor.h"
#include "extract/registry.h"

#include <iostream>
#include <string>
#include <filesystem>

static void printPlayerUsage(const std::string& prog) {
    std::cout
        << "Music Player - web UI for the audio library (C++)\n\n"
        << "Usage:\n"
        << "  " << prog << " [options]\n\n"
        << "Options:\n"
        << "  -p, --port N        HTTP port (default: 8080)\n"
        << "  -d, --dir PATH      Music library directory\n"
        << "      --bind ADDR     Bind address (default: 0.0.0.0 = 局域网可访问)\n"
        << "      --local         只监听本机（等价于 --bind 127.0.0.1）\n"
        << "      --no-firewall   不自动添加 Windows 防火墙入站规则\n"
        << "      --no-browser    Do not launch a browser automatically\n"
        << "  -h, --help          Show this help\n\n"
        << "The library directory defaults to your OS music folder and is\n"
        << "remembered between runs.\n\n"
        << "默认对所有网卡开放：同一局域网（Wi-Fi / 网线）内的手机、平板、电脑\n"
        << "都可以打开启动信息里列出的「局域网访问」地址使用本播放器。\n"
        << "界面左下角「局域网」按钮可随时查看这些地址。\n";
}

int main(int argc, char** argv) {
    std::vector<std::string> args = plat::getCommandLineArgs(argc, argv);
    const std::string prog = args.empty() ? "music_player" : args[0];

    PlayerConfig cfg;

    // Resolve the paths the server needs relative to the executable, so the
    // player also works when started from another directory.
    std::filesystem::path exeDir;
    {
        std::error_code ec;
        std::filesystem::path p = plat::u8path(prog);
        if (p.is_absolute()) exeDir = p.parent_path();
        else exeDir = std::filesystem::absolute(
                          plat::u8path("./") / plat::u8path("./"), ec) ;
        if (exeDir.empty()) exeDir = plat::u8path(".");
    }
    cfg.configPath = plat::pathToUtf8(exeDir / plat::u8path("player_config.json"));

    // Prefer a bundled web folder next to the binary; fall back to ../web for
    // in-tree builds, then to the source tree.
    std::string hereTarget = plat::pathToUtf8(exeDir / plat::u8path("web/index.html"));
    {
        std::error_code ec;
        if (std::filesystem::exists(plat::u8path(hereTarget), ec)) {
            cfg.webRoot = plat::pathToUtf8(exeDir / plat::u8path("web"));
        } else {
            cfg.webRoot = "web";
            if (!std::filesystem::exists(plat::u8path("web/index.html"), ec))
                cfg.webRoot = "../web";
        }
    }

    playerLoadConfig(cfg.configPath, cfg);

    for (size_t i = 1; i < args.size(); ++i) {
        const std::string a = args[i];
        auto needVal = [&](const char* name) -> std::string {
            if (i + 1 >= args.size()) {
                std::cerr << "Missing value for " << name << "\n";
                std::exit(2);
            }
            return args[++i];
        };
        if      (a == "-h" || a == "--help")  { printPlayerUsage(prog); return 0; }
        else if (a == "-p" || a == "--port")  cfg.port = std::atoi(needVal("--port").c_str());
        else if (a == "-d" || a == "--dir")   cfg.libraryDir = needVal("--dir");
        else if (a == "--bind")               cfg.bindAddr = needVal("--bind");
        else if (a == "--local" || a == "--localhost") cfg.bindAddr = "127.0.0.1";
        else if (a == "--lan")                cfg.bindAddr = "0.0.0.0";
        else if (a == "--no-firewall")        cfg.manageFirewall = false;
        else if (a == "--no-browser")         cfg.openBrowser = false;
        else {
            std::cerr << "Unknown option: " << a << "\n";
            printPlayerUsage(prog);
            return 2;
        }
    }

    if (cfg.libraryDir.empty()) cfg.libraryDir = "music";

    if (!HttpClient::init()) {
        std::cerr << "Failed to initialize libcurl\n";
        return 1;
    }
    ExtractorRegistry::initDefaults();
    int rc = runPlayerServer(cfg);
    playerSaveConfig(cfg.configPath, cfg);
    HttpClient::cleanup();
    return rc;
}
