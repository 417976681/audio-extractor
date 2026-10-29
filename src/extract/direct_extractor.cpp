#include "direct_extractor.h"
#include "../net/http_client.h"
#include "../common/platform.h"

#include <algorithm>

bool DirectExtractor::supports(const std::string& url) const {
    // m3u8 is handled by the HLS extractor; everything else is treated as a
    // direct media URL (the universal fallback).
    std::string lower;
    lower.reserve(url.size());
    for (char c : url) lower.push_back((char)std::tolower((unsigned char)c));
    if (lower.find(".m3u8") != std::string::npos) return false;
    if (lower.find("m3u8") != std::string::npos &&
        lower.find("?") != std::string::npos) {
        // URLs like .../playlist.m3u8?token=... are still HLS.
        if (lower.find("m3u8") < lower.find("?")) return false;
    }
    return true;
}

MediaSource DirectExtractor::extract(const std::string& url,
                                     const ExtractOptions& opt,
                                     const TempDir& tempDir) {
    // Choose a local extension that hints the container to FFmpeg.
    std::string ext = lowerExtension(url);
    if (ext.empty()) ext = "mp4";  // safe default; FFmpeg probes anyway.
    std::string outPath = tempDir.join("media." + ext);

    LOG_INFO("DirectExtractor: downloading media to " + outPath);

    long httpStatus = 0;
    long rc = HttpClient::downloadToFile(url, outPath, opt, &httpStatus);
    if (rc != CURLE_OK) {
        throw std::runtime_error("DirectExtractor: download failed (curl " +
                                 std::to_string(rc) + ")");
    }
    if (httpStatus >= 400) {
        throw std::runtime_error("DirectExtractor: HTTP " +
                                 std::to_string(httpStatus));
    }

    // Sanity check: if the server returned an HTML page instead of media,
    // we cannot proceed (this usually means the URL was a webpage, not media).
    {
        FILE* f = plat::openFileRead(outPath);
        if (!f) {
            throw std::runtime_error("DirectExtractor: cannot open downloaded file for "
                                     "inspection: " + outPath);
        }
        char buf[512] = {0};
        size_t got = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        std::string head(buf, got);
        // Trim leading whitespace.
        size_t p = head.find_first_not_of(" \t\r\n");
        if (p != std::string::npos) head = head.substr(p);
        if (head.compare(0, 1, "<") == 0 || head.find("<!doctype") != std::string::npos ||
            head.find("<html") != std::string::npos) {
            throw std::runtime_error(
                "DirectExtractor: URL returned an HTML page, not media. "
                "This looks like a website page rather than a direct media "
                "file; a site-specific extractor is required.");
        }
    }

    MediaSource src;
    src.mode = MediaSource::Mode::FILE;
    src.filePath = outPath;
    return src;
}
