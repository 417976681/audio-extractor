#include "hls_extractor.h"
#include "../net/http_client.h"
#include "../common/platform.h"

#include <sstream>
#include <algorithm>
#include <stdexcept>

bool HlsExtractor::supports(const std::string& url) const {
    std::string lower;
    for (char c : url) lower.push_back((char)std::tolower((unsigned char)c));
    return lower.find("m3u8") != std::string::npos;
}

std::string HlsExtractor::resolveUrl(const std::string& base, const std::string& rel) {
    if (rel.find("://") != std::string::npos) return rel;
    if (!rel.empty() && rel[0] == '/') {
        auto pos = base.find("://");
        if (pos == std::string::npos) return rel;
        std::string scheme = base.substr(0, pos);
        std::string rest = base.substr(pos + 3);
        auto slash = rest.find('/');
        std::string host = (slash == std::string::npos) ? rest : rest.substr(0, slash);
        return scheme + "://" + host + rel;
    }
    // Relative: strip to last '/'.
    std::string dir = base;
    auto q = dir.find('?');
    if (q != std::string::npos) dir = dir.substr(0, q);
    auto h = dir.find_last_of('/');
    if (h != std::string::npos) dir = dir.substr(0, h + 1);
    else dir = "";
    std::string out = dir + rel;
    // Normalize "./" and "../".
    size_t pos;
    while ((pos = out.find("/./")) != std::string::npos) out.replace(pos, 3, "/");
    while ((pos = out.find("/../")) != std::string::npos) {
        if (pos == 0) { out.replace(pos, 3, "/"); continue; }
        size_t prev = out.find_last_of('/', pos - 1);
        if (prev == std::string::npos) out.replace(0, pos + 3, "");
        else out.replace(prev, pos - prev + 3, "/");
    }
    return out;
}

std::string HlsExtractor::resolvePlaylist(const std::string& url,
                                          const ExtractOptions& opt,
                                          std::string& outBaseUrl) {
    std::string text;
    long httpStatus = 0;
    long rc = HttpClient::downloadToString(url, text, opt, &httpStatus);
    if (rc != CURLE_OK)
        throw std::runtime_error("HlsExtractor: failed to fetch manifest (curl " +
                                 std::to_string(rc) + ")");
    if (httpStatus >= 400)
        throw std::runtime_error("HlsExtractor: manifest HTTP " +
                                 std::to_string(httpStatus));

    outBaseUrl = url;

    // Master playlist? (#EXT-X-STREAM-INF without #EXTINF)
    bool isMaster = text.find("#EXT-X-STREAM-INF") != std::string::npos;
    if (!isMaster) return text;

    // Pick the variant with the highest BANDWIDTH.
    std::istringstream iss(text);
    std::string line;
    long bestBandwidth = -1;
    std::string bestUri;
    while (std::getline(iss, line)) {
        if (line.find("#EXT-X-STREAM-INF") == 0) {
            long bw = 0;
            auto p = line.find("BANDWIDTH");
            if (p != std::string::npos) {
                auto eq = line.find('=', p);
                auto comma = line.find_first_of(", \t", eq + 1);
                std::string num = line.substr(eq + 1, comma - (eq + 1));
                try { bw = std::stol(num); } catch (...) { bw = 0; }
            }
            std::string uriLine;
            if (std::getline(iss, uriLine)) {
                // Trim whitespace.
                size_t a = uriLine.find_first_not_of(" \t\r\n");
                size_t b = uriLine.find_last_not_of(" \t\r\n");
                if (a != std::string::npos) uriLine = uriLine.substr(a, b - a + 1);
                if (!uriLine.empty() && uriLine[0] != '#' && bw >= bestBandwidth) {
                    bestBandwidth = bw;
                    bestUri = uriLine;
                }
            }
        }
    }
    if (bestUri.empty())
        throw std::runtime_error("HlsExtractor: no variant found in master playlist");

    std::string variantUrl = resolveUrl(url, bestUri);
    LOG_INFO("HlsExtractor: selected variant (bandwidth=" + std::to_string(bestBandwidth) + ")");
    // Recurse in case the variant is itself a master playlist (rare).
    return resolvePlaylist(variantUrl, opt, outBaseUrl);
}

MediaSource HlsExtractor::extract(const std::string& url,
                                  const ExtractOptions& opt,
                                  const TempDir& tempDir) {
    std::string baseUrl;
    std::string playlist = resolvePlaylist(url, opt, baseUrl);

    std::istringstream iss(playlist);
    std::string line;
    std::vector<std::string> segments;
    double totalDuration = 0.0;
    bool encrypted = false;

    while (std::getline(iss, line)) {
        if (line.find("#EXT-X-KEY") != std::string::npos) {
            if (line.find("METHOD=NONE") == std::string::npos) encrypted = true;
            continue;
        }
        if (line.find("#EXTINF") == 0) {
            // #EXTINF:<duration>,<title>
            auto colon = line.find(':');
            if (colon != std::string::npos) {
                auto comma = line.find(',', colon + 1);
                std::string dur = line.substr(colon + 1, comma - (colon + 1));
                try { totalDuration += std::stod(dur); } catch (...) {}
            }
            continue;
        }
        if (line.empty() || line[0] == '#') continue;
        // Segment URI.
        segments.push_back(resolveUrl(baseUrl, line));
    }

    if (segments.empty())
        throw std::runtime_error("HlsExtractor: no segments found in playlist");

    if (encrypted)
        LOG_WARN("HlsExtractor: playlist is encrypted; segments will be "
                 "downloaded but NOT decrypted in this version.");

    LOG_INFO("HlsExtractor: downloading " + std::to_string(segments.size()) +
             " segment(s) (" + std::to_string((long)totalDuration) + "s)");

    std::vector<std::string> localPaths;
    localPaths.reserve(segments.size());
    int idx = 0;
    int done = 0;
    for (const auto& segUrl : segments) {
        std::string name = "seg_" + std::to_string(idx) + ".ts";
        std::string local = tempDir.join(name);
        long httpStatus = 0;
        long rc = HttpClient::downloadToFile(segUrl, local, opt, &httpStatus);
        if (rc != CURLE_OK || httpStatus >= 400) {
            throw std::runtime_error("HlsExtractor: failed to download segment " +
                                     std::to_string(idx) + " (curl " +
                                     std::to_string(rc) + ", http " +
                                     std::to_string(httpStatus) + ")");
        }
        localPaths.push_back(local);
        ++idx;
        ++done;
        if (opt.progress) opt.progress((double)done / (double)segments.size());
    }

    // Build concat list file with absolute paths.
    std::string listPath = tempDir.join("segments.txt");
    {
        FILE* out = plat::openFileWrite(listPath);
        if (!out) throw std::runtime_error("HlsExtractor: cannot write concat list");
        for (const auto& p : localPaths) {
            std::string line = "file '" + p + "'\n";
            fwrite(line.data(), 1, line.size(), out);
        }
        fclose(out);
    }

    MediaSource src;
    src.mode = MediaSource::Mode::CONCAT;
    src.concatListPath = listPath;
    src.durationHint = totalDuration;
    return src;
}
