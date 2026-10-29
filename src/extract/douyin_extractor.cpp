#include "douyin_extractor.h"
#include "../net/http_client.h"
#include "../common/json.h"
#include "../common/platform.h"

#include <string>
#include <vector>
#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <filesystem>

namespace {

std::string toLower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// Pull the numeric aweme id out of a Douyin URL. Supports:
//   https://v.douyin.com/XXXX/            (short link — resolved separately)
//   https://www.douyin.com/video/7300000000000000000
//   https://www.iesdouyin.com/share/video/7300000000000000000/
//   https://www.douyin.com/discover?modal_id=7300000000000000000
std::string extractAwemeId(const std::string& url) {
    std::string low = toLower(url);
    // explicit /video/<id> or /share/video/<id>/
    for (const char* seg : {"/video/", "/share/video/"}) {
        size_t s = low.find(seg);
        if (s != std::string::npos) {
            std::string rest = url.substr(s + std::string(seg).size());
            std::string id;
            for (char c : rest) {
                if (std::isdigit((unsigned char)c)) id.push_back(c);
                else break;
            }
            if (!id.empty()) return id;
        }
    }
    // ?modal_id=<id>
    size_t m = low.find("modal_id=");
    if (m != std::string::npos) {
        std::string rest = url.substr(m + 10);
        std::string id;
        for (char c : rest) {
            if (std::isdigit((unsigned char)c)) id.push_back(c);
            else break;
        }
        if (!id.empty()) return id;
    }
    return "";
}

std::string sanitizeName(const std::string& in) {
    std::string out;
    for (char c : in) {
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|')
            out.push_back('_');
        else
            out.push_back(c);
    }
    while (!out.empty() &&
           (out.back() == ' ' || out.back() == '.' || out.back() == '\t'))
        out.pop_back();
    return out.empty() ? "douyin" : out;
}

// Collect every URL from a JSON array of strings (Douyin url_list fields).
void collectUrlList(const JsonValue* node, std::vector<std::string>& out) {
    if (!node || !node->isArray()) return;
    for (const auto& e : node->arr) {
        if (e.isString() && !e.asString().empty()) out.push_back(e.asString());
    }
}

}  // namespace

bool DouyinExtractor::supports(const std::string& url) const {
    std::string low = toLower(url);
    return low.find("douyin.com") != std::string::npos ||
           low.find("iesdouyin.com") != std::string::npos;
}

MediaSource DouyinExtractor::extract(const std::string& url,
                                     const ExtractOptions& opt,
                                     const TempDir& tempDir) {
    ExtractOptions dopt = opt;
    dopt.referer = "https://www.douyin.com/";
    dopt.connectTimeoutSec = 20;
    dopt.timeoutSec = 45;

    // --- resolve short links (v.douyin.com/XXXX -> long video page) --------
    std::string workUrl = url;
    {
        std::string low = toLower(url);
        if (low.find("v.douyin.com") != std::string::npos) {
            std::string finalUrl;
            if (HttpClient::resolveRedirect(url, dopt, finalUrl) == CURLE_OK &&
                !finalUrl.empty()) {
                workUrl = finalUrl;
                LOG_INFO("DouyinExtractor: short link -> " + workUrl);
            }
        }
    }

    std::string awemeId = extractAwemeId(workUrl);
    if (awemeId.empty())
        throw std::runtime_error("DouyinExtractor: could not find an aweme id in the URL");
    LOG_INFO("DouyinExtractor: awemeId=" + awemeId);

    // --- iteminfo API ------------------------------------------------------
    std::vector<std::pair<std::string, std::string>> hdrs = {
        {"User-Agent",
         "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
         "(KHTML, like Gecko) Chrome/120.0 Safari/537.36"},
        {"Referer", "https://www.douyin.com/"},
        {"Accept", "application/json"},
    };
    std::string api =
        "https://www.iesdouyin.com/web/api/v2/aweme/iteminfo/?item_ids=" + awemeId;
    // Allow redirecting the endpoint for local testing / debugging.
    const char* override = std::getenv("DOUYIN_API_BASE");
    if (override) api = std::string(override) + "?item_ids=" + awemeId;
    std::string resp;
    long st = 0;
    long rc = HttpClient::postToString(api, "", dopt, resp, &st, &hdrs);
    if (rc != CURLE_OK)
        throw std::runtime_error("DouyinExtractor: iteminfo request failed (curl " +
                                 std::to_string(rc) + ")");
    if (st >= 400)
        throw std::runtime_error("DouyinExtractor: iteminfo HTTP " + std::to_string(st));

    JsonValue root;
    try {
        root = JsonParser::parse(resp);
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("DouyinExtractor: failed to parse "
                                            "iteminfo response (") +
                                e.what() + ")");
    }

    const JsonValue* list = root.find("aweme_list");
    if (!list || !list->isArray() || list->arr.empty())
        throw std::runtime_error(
            "DouyinExtractor: iteminfo returned no aweme_list — Douyin likely "
            "rejected the unsigned request (X-Bogus signature required).");

    const JsonValue* aweme = list->arr[0].isObject() ? &list->arr[0] : nullptr;
    if (!aweme)
        throw std::runtime_error("DouyinExtractor: malformed aweme entry");

    std::string title = "douyin-" + awemeId;
    const JsonValue* desc = aweme->find("desc");
    if (desc && desc->isString() && !desc->asString().empty())
        title = sanitizeName(desc->asString());

    // Prefer the original music track, then the video's own audio stream.
    std::vector<std::string> candidates;
    const JsonValue* music = aweme->find("music");
    if (music && music->isObject()) {
        const JsonValue* pu = music->find("play_url");
        if (pu && pu->isObject()) collectUrlList(pu->find("url_list"), candidates);
    }
    const JsonValue* video = aweme->find("video");
    if (video && video->isObject()) {
        const JsonValue* pa = video->find("play_addr");
        if (pa && pa->isObject()) collectUrlList(pa->find("url_list"), candidates);
        // Some responses nest play_addr under bit_rate entries.
        const JsonValue* br = video->find("bit_rate");
        if (candidates.empty() && br && br->isArray()) {
            for (const auto& b : br->arr) {
                if (!b.isObject()) continue;
                const JsonValue* pa2 = b.find("play_addr");
                if (pa2 && pa2->isObject())
                    collectUrlList(pa2->find("url_list"), candidates);
            }
        }
    }

    if (candidates.empty())
        throw std::runtime_error("DouyinExtractor: no playable URL found in aweme");

    std::string outPath = tempDir.join("douyin_media.mp4");
    for (const auto& cand : candidates) {
        long localSt = 0;
        long r = HttpClient::downloadToFile(cand, outPath, dopt, &localSt);
        bool ok = (r == CURLE_OK && localSt < 400 &&
                   std::filesystem::file_size(plat::u8path(outPath)) > 0);
        if (ok) {
            LOG_INFO("DouyinExtractor: downloaded media (" +
                     std::to_string(candidates.size()) + " candidate(s))");
            MediaSource src;
            src.mode = MediaSource::Mode::FILE;
            src.filePath = outPath;
            src.suggestedName = title;
            return src;
        }
        LOG_WARN("DouyinExtractor: media download failed (curl " +
                 std::to_string(r) + ", http " + std::to_string(localSt) +
                 "); trying next candidate");
    }

    throw std::runtime_error("DouyinExtractor: all media URLs failed to download "
                             "(they may have expired or require a signed request)");
}
