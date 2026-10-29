#include "bilibili_extractor.h"
#include "../net/http_client.h"
#include "../common/json.h"
#include "../common/md5.h"
#include "../common/platform.h"

#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <ctime>
#include <cctype>
#include <filesystem>

namespace {

// --- small helpers ---------------------------------------------------------

std::string toLower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// Percent-encode a string for use in a query component (RFC 3986 unreserved
// set is left intact; everything else becomes %XX).
std::string urlEncode(const std::string& in) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : in) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back((char)c);
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0xF]);
        }
    }
    return out;
}

// Extract the BVID (BV...) from a Bilibili URL, preferring the /video/ path.
std::string extractBvid(const std::string& url) {
    std::string low = toLower(url);
    std::string scope = url;
    size_t vp = low.find("/video/");
    if (vp != std::string::npos) {
        scope = url.substr(vp + 7);
        low = low.substr(vp + 7);
    }
    size_t p = low.find("bv");
    if (p != std::string::npos) {
        size_t k = p + 2;
        std::string bv = "BV";
        while (k < scope.size() && std::isalnum((unsigned char)scope[k]) && bv.size() < 12) {
            bv.push_back(scope[k]);
            ++k;
        }
        if (bv.size() >= 10) return bv;
    }
    return "";
}

// Extract the key portion (filename without .png) from a WBI image/sub URL.
std::string extractWbiKey(const std::string& url) {
    size_t slash = url.find_last_of('/');
    std::string fname = (slash != std::string::npos) ? url.substr(slash + 1) : url;
    if (fname.size() >= 4) {
        std::string ext = toLower(fname.substr(fname.size() - 4));
        if (ext == ".png") fname = fname.substr(0, fname.size() - 4);
    }
    return fname;
}

bool looksLikeHtml(const std::string& path) {
    FILE* f = plat::openFileRead(path);
    if (!f) return true;
    char buf[512] = {0};
    size_t got = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    std::string head(buf, got);
    size_t p = head.find_first_not_of(" \t\r\n");
    if (p != std::string::npos) head = head.substr(p);
    return head.compare(0, 1, "<") == 0 ||
           head.find("<!doctype") != std::string::npos ||
           head.find("<html") != std::string::npos;
}

// WBI mixin permutation table (fixed by Bilibili).
const int WBI_MIXIN[64] = {
    46, 47, 18, 2, 53, 8, 23, 32, 15, 50, 10, 31, 58, 3, 45, 35,
    27, 43, 5, 49, 33, 9, 42, 19, 29, 28, 14, 39, 12, 38, 41, 13,
    37, 48, 7, 16, 24, 55, 40, 61, 26, 17, 0, 1, 60, 51, 30, 4,
    22, 25, 54, 21, 56, 59, 6, 63, 57, 62, 11, 36, 20, 34, 44, 52
};

}  // namespace

bool BilibiliExtractor::supports(const std::string& url) const {
    std::string low = toLower(url);
    if (low.find("bilibili.com") == std::string::npos &&
        low.find("b23.tv") == std::string::npos)
        return false;
    // Only handle watch pages here; let other URL shapes fall through.
    return low.find("/video/") != std::string::npos ||
           low.find("b23.tv") != std::string::npos;
}

MediaSource BilibiliExtractor::extract(const std::string& url,
                                      const ExtractOptions& opt,
                                      const TempDir& tempDir) {
    const std::string REFERER = "https://www.bilibili.com";

    ExtractOptions bopt = opt;
    bopt.referer = REFERER;

    // --- resolve short links (b23.tv/<code> -> bilibili video page) --------
    std::string workUrl = url;
    {
        std::string low = toLower(url);
        if (low.find("b23.tv") != std::string::npos) {
            std::string finalUrl;
            if (HttpClient::resolveRedirect(url, bopt, finalUrl) == CURLE_OK &&
                !finalUrl.empty()) {
                std::string bv = extractBvid(finalUrl);
                if (!bv.empty()) {
                    workUrl = "https://www.bilibili.com/video/" + bv;
                    LOG_INFO("BilibiliExtractor: short link resolved -> " + workUrl);
                } else {
                    workUrl = finalUrl;  // fall back to whatever we landed on
                }
            }
        }
    }

    std::string bvid = extractBvid(workUrl);
    if (bvid.empty())
        throw std::runtime_error("BilibiliExtractor: could not find a BV id in the URL");
    LOG_INFO("BilibiliExtractor: bvid=" + bvid);

    // --- page number (?p=N) -------------------------------------------------
    long pnum = 1;
    size_t q = workUrl.find('?');
    if (q != std::string::npos) {
        std::string qs = url.substr(q + 1);
        size_t pp = qs.find("p=");
        if (pp != std::string::npos) {
            std::string rest = qs.substr(pp + 2);
            size_t amp = rest.find('&');
            if (amp != std::string::npos) rest = rest.substr(0, amp);
            try { pnum = std::stol(rest); } catch (...) { pnum = 1; }
            if (pnum < 1) pnum = 1;
        }
    }

    long (*dlStr)(const std::string&, std::string&, const ExtractOptions&, long*) =
        HttpClient::downloadToString;

    // --- 1) view -> cid -----------------------------------------------------
    std::string viewJson;
    long st = 0;
    long rc = dlStr("https://api.bilibili.com/x/web-interface/view?bvid=" + bvid,
                    viewJson, bopt, &st);
    if (rc != CURLE_OK)
        throw std::runtime_error("BilibiliExtractor: view request failed (curl " +
                                 std::to_string(rc) + ")");
    if (st >= 400)
        throw std::runtime_error("BilibiliExtractor: view HTTP " + std::to_string(st));

    JsonValue view = JsonParser::parse(viewJson);
    const JsonValue* data = view.find("data");
    if (!data || !data->isObject()) {
        long code = view.find("code") ? view.find("code")->asLong() : -1;
        throw std::runtime_error("BilibiliExtractor: view returned no data (code " +
                                 std::to_string(code) + ")");
    }
    // NOTE: Bilibili cids exceed 2^31, so a 32-bit `long` would truncate them
    // on Windows/MinGW. Use long long (64-bit) everywhere an id is stored.
    long long cid = 0;
    if (pnum <= 1) {
        const JsonValue* c = data->find("cid");
        if (c && c->isNumber()) cid = c->asLong();
    } else {
        const JsonValue* pages = data->find("pages");
        if (pages && pages->isArray() && (long)pages->arr.size() >= pnum) {
            const JsonValue* pg = pages->at((size_t)pnum - 1);
            const JsonValue* c = pg ? pg->find("cid") : nullptr;
            if (c && c->isNumber()) cid = c->asLong();
        }
        if (cid == 0) {
            const JsonValue* c = data->find("cid");
            if (c && c->isNumber()) cid = c->asLong();
        }
    }
    if (cid == 0)
        throw std::runtime_error("BilibiliExtractor: could not determine cid");
    LOG_INFO("BilibiliExtractor: cid=" + std::to_string(cid));

    // Remember the video/part title so the caller can name the output file.
    std::string suggestedName;
    {
        const JsonValue* t = data->find("title");
        if (t && t->isString()) suggestedName = t->asString();
        if (pnum > 1) {
            const JsonValue* pages = data->find("pages");
            if (pages && pages->isArray() && (long)pages->arr.size() >= pnum) {
                const JsonValue* pg = pages->at((size_t)pnum - 1);
                const JsonValue* pt = pg ? pg->find("part") : nullptr;
                if (pt && pt->isString() && !pt->asString().empty())
                    suggestedName += " - P" + std::to_string(pnum) + " " + pt->asString();
            }
        }
    }
    LOG_INFO("BilibiliExtractor: title=" + suggestedName);

    // --- 2) finger/spi -> buvid3 / buvid4 ----------------------------------
    std::string spiJson;
    rc = dlStr("https://api.bilibili.com/x/frontend/finger/spi", spiJson, bopt, &st);
    if (rc != CURLE_OK)
        throw std::runtime_error("BilibiliExtractor: spi request failed (curl " +
                                 std::to_string(rc) + ")");
    std::string b3, b4;
    try {
        JsonValue spi = JsonParser::parse(spiJson);
        const JsonValue* d = spi.find("data");
        if (d && d->isObject()) {
            const JsonValue* a = d->find("b_3");
            if (a && a->isString()) b3 = a->asString();
            const JsonValue* b = d->find("b_4");
            if (b && b->isString()) b4 = b->asString();
        }
    } catch (...) { /* best-effort */ }
    if (b3.empty())
        LOG_WARN("BilibiliExtractor: no buvid3 obtained from spi; continuing");

    std::string biliCookie;
    if (!opt.cookie.empty()) biliCookie = opt.cookie + "; ";
    biliCookie += "buvid3=" + b3 + "; buvid4=" + b4;
    ExtractOptions bopt2 = bopt;
    bopt2.cookie = biliCookie;

    // --- 3) nav -> WBI keys ------------------------------------------------
    std::string navJson;
    rc = dlStr("https://api.bilibili.com/x/web-interface/nav", navJson, bopt2, &st);
    if (rc != CURLE_OK)
        throw std::runtime_error("BilibiliExtractor: nav request failed (curl " +
                                 std::to_string(rc) + ")");
    std::string imgKey, subKey;
    try {
        JsonValue nav = JsonParser::parse(navJson);
        const JsonValue* d = nav.find("data");
        if (d && d->isObject()) {
            const JsonValue* wbi = d->find("wbi_img");
            if (wbi && wbi->isObject()) {
                const JsonValue* iu = wbi->find("img_url");
                const JsonValue* su = wbi->find("sub_url");
                if (iu && iu->isString()) imgKey = extractWbiKey(iu->asString());
                if (su && su->isString()) subKey = extractWbiKey(su->asString());
            }
        }
    } catch (...) { /* best-effort */ }
    if (imgKey.empty() || subKey.empty())
        throw std::runtime_error("BilibiliExtractor: failed to obtain WBI keys "
                                 "(is the network reachable? are cookies valid?)");

    // --- 4) mixin key -------------------------------------------------------
    std::string orig = imgKey + subKey;
    std::string mixinKey;
    for (int i = 0; i < 64; ++i)
        if ((size_t)WBI_MIXIN[i] < orig.size())
            mixinKey += orig[WBI_MIXIN[i]];
    mixinKey = mixinKey.substr(0, 32);

    // --- 5) sign + playurl -------------------------------------------------
    std::vector<std::pair<std::string, std::string>> params = {
        {"bvid", bvid},
        {"cid", std::to_string(cid)},
        {"fnval", "16"},   // request DASH
        {"qn", "127"},     // highest quality
        {"fourk", "1"},
        {"wts", std::to_string((long long)std::time(nullptr))}
    };
    std::sort(params.begin(), params.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    std::string query;
    for (size_t i = 0; i < params.size(); ++i) {
        if (i) query += '&';
        query += urlEncode(params[i].first) + '=' + urlEncode(params[i].second);
    }
    std::string wrid = md5Hex(query + mixinKey);
    std::string playUrl = "https://api.bilibili.com/x/player/wbi/playurl?" +
                          query + "&w_rid=" + wrid;

    std::string plJson;
    rc = dlStr(playUrl, plJson, bopt2, &st);
    if (rc != CURLE_OK)
        throw std::runtime_error("BilibiliExtractor: playurl request failed (curl " +
                                 std::to_string(rc) + ")");
    if (st >= 400)
        throw std::runtime_error("BilibiliExtractor: playurl HTTP " + std::to_string(st));
    JsonValue pl = JsonParser::parse(plJson);
    const JsonValue* pld = pl.find("data");
    if (!pld || !pld->isObject()) {
        long code = pl.find("code") ? pl.find("code")->asLong() : -1;
        std::string msg = pl.find("message") ? pl.find("message")->asString() : "";
        throw std::runtime_error("BilibiliExtractor: playurl returned no data (code " +
                                 std::to_string(code) + ", msg: " + msg + ")");
    }
    const JsonValue* dash = pld->find("dash");
    if (!dash || !dash->isObject())
        throw std::runtime_error("BilibiliExtractor: no DASH stream (video may be "
                                 "region-locked or require login / VIP)");

    // Choose an audio stream. Preference: Dolby > FLAC (lossless) > best DASH audio.
    std::string chosenUrl;
    std::vector<std::string> backups;
    auto grab = [&](const JsonValue* o) {
        if (!o || !o->isObject()) return;
        const JsonValue* bu = o->find("baseUrl");
        if (bu && bu->isString() && !bu->asString().empty()) chosenUrl = bu->asString();
        const JsonValue* bu2 = o->find("base_url");
        if (chosenUrl.empty() && bu2 && bu2->isString() && !bu2->asString().empty())
            chosenUrl = bu2->asString();
        const JsonValue* bk = o->find("backupUrl");
        if (bk && bk->isArray())
            for (const auto& e : bk->arr)
                if (e.isString() && !e.asString().empty()) backups.push_back(e.asString());
        const JsonValue* bk2 = o->find("backup_url");
        if (backups.empty() && bk2 && bk2->isArray())
            for (const auto& e : bk2->arr)
                if (e.isString() && !e.asString().empty()) backups.push_back(e.asString());
    };

    const JsonValue* dolby = dash->find("dolby");
    if (dolby && dolby->isObject()) grab(dolby->find("audio"));
    if (chosenUrl.empty()) {
        const JsonValue* flac = dash->find("flac");
        if (flac && flac->isObject()) grab(flac->find("audio"));
    }
    if (chosenUrl.empty()) {
        const JsonValue* audio = dash->find("audio");
        if (audio && audio->isArray() && !audio->arr.empty()) {
            const JsonValue* best = nullptr;
            long long bestBw = -1;
            for (const auto& e : audio->arr) {
                if (!e.isObject()) continue;
                const JsonValue* bw = e.find("bandwidth");
                long long b = (bw && bw->isNumber()) ? bw->asLong() : 0;
                if (b > bestBw) { bestBw = b; best = &e; }
            }
            if (best) grab(best);
        }
    }
    if (chosenUrl.empty())
        throw std::runtime_error("BilibiliExtractor: no audio stream found in DASH");
    LOG_INFO("BilibiliExtractor: selected audio stream (" +
             std::to_string(backups.size()) + " backup URL(s))");

    // --- 6) download audio to a local file ---------------------------------
    std::string outPath = tempDir.join("bili_audio.m4a");
    std::vector<std::string> candidates;
    candidates.push_back(chosenUrl);
    candidates.insert(candidates.end(), backups.begin(), backups.end());

    bool ok = false;
    for (const auto& cand : candidates) {
        long localSt = 0;
        long r = HttpClient::downloadToFile(cand, outPath, bopt2, &localSt);
        if (r == CURLE_OK && localSt < 400 &&
            std::filesystem::file_size(plat::u8path(outPath)) > 0 && !looksLikeHtml(outPath)) {
            ok = true;
            break;
        }
        LOG_WARN("BilibiliExtractor: audio download failed (curl " + std::to_string(r) +
                 ", http " + std::to_string(localSt) + "); trying backup");
    }
    if (!ok)
        throw std::runtime_error("BilibiliExtractor: failed to download audio stream");

    MediaSource src;
    src.mode = MediaSource::Mode::FILE;
    src.filePath = outPath;
    src.suggestedName = suggestedName;
    return src;
}
