#pragma once

#include <string>
#include <functional>
#include <iostream>
#include <filesystem>
#include <cstdlib>
#include <cctype>
#include "common/platform.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------
enum class LogLevel { INFO, WARN, ERR };

inline void logMsg(LogLevel level, const std::string& msg) {
    const char* tag = "INFO";
    if (level == LogLevel::WARN) tag = "WARN";
    else if (level == LogLevel::ERR) tag = "ERROR";
    std::cerr << "[" << tag << "] " << msg << std::endl;
}

#define LOG_INFO(...)  logMsg(LogLevel::INFO, __VA_ARGS__)
#define LOG_WARN(...)  logMsg(LogLevel::WARN, __VA_ARGS__)
#define LOG_ERROR(...) logMsg(LogLevel::ERR,  __VA_ARGS__)

// ---------------------------------------------------------------------------
// Progress callback. fraction is in [0,1]; a value < 0 means "unknown".
// ---------------------------------------------------------------------------
using ProgressCb = std::function<void(double fraction)>;

// ---------------------------------------------------------------------------
// Options shared by all extractors (HTTP headers / cookies / progress).
// ---------------------------------------------------------------------------
struct ExtractOptions {
    std::string userAgent = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                            "AppleWebKit/537.36 (KHTML, like Gecko) "
                            "Chrome/120.0 Safari/537.36";
    std::string referer;
    std::string cookie;
    ProgressCb progress;

    // Per-request timeouts in seconds. 0 means "use the transport default",
    // which suits large media downloads. Short-lived metadata calls (lyric
    // lookups, signature keys) set these explicitly so one dead endpoint
    // cannot stall the caller.
    long connectTimeoutSec = 0;
    long timeoutSec = 0;
};

// ---------------------------------------------------------------------------
// RAII temporary directory. Removed (recursively) on destruction.
// ---------------------------------------------------------------------------
class TempDir {
public:
    explicit TempDir(const std::string& prefix = "ae_tmp") {
        std::error_code ec;
        std::filesystem::path base = std::filesystem::temp_directory_path(ec);
        if (ec) base = ".";
        for (int attempt = 0; attempt < 1000; ++attempt) {
            std::filesystem::path cand =
                base / (prefix + "_" + std::to_string(GetCurrentProcessId())
                        + "_" + std::to_string(attempt) + "_"
                        + std::to_string(std::rand()));
            if (!std::filesystem::exists(cand, ec)) {
                std::filesystem::create_directories(cand, ec);
                if (!ec) { path_ = cand; own_ = true; return; }
            }
        }
        throw std::runtime_error("TempDir: failed to create unique directory");
    }

    ~TempDir() {
        if (own_) {
            std::error_code ec;
            std::filesystem::remove_all(path_, ec);
        }
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    std::string path() const { return plat::pathToUtf8(path_); }
    std::string join(const std::string& name) const {
        return plat::pathToUtf8(path_ / plat::u8path(name));
    }

private:
    std::filesystem::path path_;
    bool own_ = false;
};

// Small helper to read a file's extension (lowercased), e.g. "mp3".
inline std::string lowerExtension(const std::string& path) {
    std::string ext = plat::pathToUtf8(plat::u8path(path).extension());
    if (!ext.empty() && ext[0] == '.') ext = ext.substr(1);
    for (char& c : ext) c = (char)std::tolower((unsigned char)c);
    return ext;
}

inline void reportProgress(const ProgressCb& cb, double fraction) {
    if (cb) cb(fraction);
}
