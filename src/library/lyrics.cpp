#include "lyrics.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>

#include "common/common.h"
#include "common/json.h"
#include "common/platform.h"
#include "net/http_client.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
}

namespace lyr {

// ===========================================================================
// helpers
// ===========================================================================

namespace {

std::string trimStr(const std::string& s) {
    auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    std::size_t a = 0;
    while (a < s.size() && !notSpace((unsigned char)s[a])) ++a;
    std::size_t b = s.size();
    while (b > a && !notSpace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}

std::string lowerStr(const std::string& s) {
    std::string r = s;
    for (char& c : r) c = (char)std::tolower((unsigned char)c);
    return r;
}

// Collapse everything that does not identify a song: case, whitespace,
// punctuation and parenthetical remarks like "(Live)", "[Remastered]".
std::string normKey(const std::string& raw) {
    std::string s = lowerStr(raw);
    // Drop bracketed segments (they are qualifiers, not names).
    std::string out;
    int depth = 0;
    for (char c : s) {
        if (c == '(' || c == '[' || c == '{') { depth++; continue; }
        if (c == ')' || c == ']' || c == '}') { depth = std::max(0, depth - 1); continue; }
        if (depth > 0) continue;
        out += c;
    }
    s = out;
    out.clear();
    for (char c : s) {
        if (std::isalnum((unsigned char)c)) out += c;
        // Keep CJK / non-ASCII bytes verbatim; they carry the real meaning.
        else if ((unsigned char)c >= 0x80) out += c;
    }
    // Trim common filler words that differ between providers.
    static const std::vector<std::string> drop = {
        "feat", "featuring", "ft", "with", "remix", "radioedit", "explicit",
        "hd", "hq", "mq", "sq", "live", "official", "mv"
    };
    // Only strips whole-token matches at the tail, cheap and predictable.
    (void)drop;
    return out;
}

// Dice coefficient over character bigrams — works for CJK as well as Latin.
double diceSim(const std::string& a, const std::string& b) {
    if (a.empty() || b.empty()) return 0.0;
    if (a == b) return 1.0;
    if (a.find(b) != std::string::npos || b.find(a) != std::string::npos) {
        double mn = (double)std::min(a.size(), b.size());
        double mx = (double)std::max(a.size(), b.size());
        return 0.85 + 0.15 * (mn / mx);
    }
    std::map<std::string, int> freq;
    if (a.size() < 2 || b.size() < 2) return 0.0;
    for (std::size_t i = 0; i + 1 < a.size(); ++i) freq[a.substr(i, 2)]++;
    double hits = 0.0;
    for (std::size_t i = 0; i + 1 < b.size(); ++i) {
        auto it = freq.find(b.substr(i, 2));
        if (it != freq.end() && it->second > 0) { hits++; it->second--; }
    }
    double total = (double)(a.size() - 1) + (double)(b.size() - 1);
    return total > 0 ? (2.0 * hits) / total : 0.0;
}

// Many providers mostly serve covers, karaoke and piano renditions once the
// original is region-locked. Those differ audibly and often re-word lines,
// so they are pushed down rather than treated as equals.
double coverPenalty(const std::string& rawTitle, const std::string& rawAlbum) {
    std::string s = lowerStr(rawTitle + " " + rawAlbum);
    static const std::vector<std::pair<std::string, double>> marks = {
        {"cover", 10.0},   {"翻唱", 10.0},     {"深情版", 9.0},   {"女声版", 9.0},
        {"男声版", 9.0},   {"钢琴版", 8.0},    {"吉他版", 8.0},   {"伴奏", 12.0},
        {"inst.", 12.0},   {"instrumental", 12.0}, {"remix", 7.0}, {"dj", 10.0},
        {"原唱", 6.0},     {"covered", 10.0},  {"致敬", 8.0},     {"type beat", 14.0},
        {"纯音乐", 12.0},  {"小提琴", 8.0},    {"电音", 8.0},     {"升调", 7.0},
        {"降调", 7.0},     {"和声伴奏", 12.0}, {"清唱", 6.0},     {"弹唱", 6.0},
    };
    double pen = 0.0;
    for (const auto& m : marks)
        if (s.find(m.first) != std::string::npos) pen += m.second;
    return std::min(pen, 18.0);
}

double scoreHit(const Query& q, const std::string& title, const std::string& artist,
                const std::string& album, double duration) {
    double s = 0.0;
    s += 60.0 * diceSim(normKey(q.title), normKey(title));

    if (!q.artist.empty() && !artist.empty()) {
        double ps = diceSim(normKey(q.artist), normKey(artist));
        s += 25.0 * ps;
        // A completely different performer almost always means a cover whose
        // transcription drifts from the original wording.
        if (ps < 0.34) s -= 12.0;
        else if (ps < 0.7) s -= 4.0;
    }
    if (!q.album.empty() && !album.empty())
        s += 10.0 * diceSim(normKey(q.album), normKey(album));
    if (q.duration > 0 && duration > 0) {
        double diff = std::fabs(q.duration - duration);
        if (diff <= 2.0) s += 5.0;
        else if (diff <= 5.0) s += 3.0;
        else if (diff <= 12.0) s += 1.0;
    }
    s -= coverPenalty(title, album);
    return s;
}

// Extract the dance: [mm:ss.xx] is the common form, but providers also emit
// [mm:ss:xx] and bare [mm:ss]. Returns false when the tag is not a timestamp.
bool parseTimeTag(const std::string& body, double* outSec) {
    // body is the text *inside* the brackets, e.g. "00:12.34".
    std::vector<std::string> parts;
    std::string cur;
    for (char c : body) {
        if (c == ':' || c == '.') { parts.push_back(cur); cur.clear(); }
        else cur += c;
    }
    parts.push_back(cur);
    if (parts.size() < 2) return false;

    auto isNum = [](const std::string& s) {
        if (s.empty()) return false;
        for (char c : s) if (!std::isdigit((unsigned char)c)) return false;
        return true;
    };
    for (const auto& p : parts)
        if (!isNum(p)) return false;

    double m = std::strtod(parts[0].c_str(), nullptr);
    double sec = std::strtod(parts[1].c_str(), nullptr);
    double frac = 0.0;
    if (parts.size() >= 3) {
        frac = std::strtod(parts[2].c_str(), nullptr);
        double denom = 1.0;
        for (std::size_t i = 0; i < parts[2].size(); ++i) denom *= 10.0;
        frac = parts[2].empty() ? 0.0 : frac / denom;
    }
    *outSec = m * 60.0 + sec + frac;
    return true;
}

// Distribution boilerplate that providers append after the last real line:
// legal notices and production credits. Worthless on a lyric screen.
bool isBoilerplate(const std::string& raw) {
    static const std::vector<std::string> needles = {
        "未经著作权人许可",
        "不得翻唱",
        "未经授权",
        "禁止用于任何人工智能",
        "AI的训练",
        "侵权",
        "icense",                       // "LICENSE"/"license" in both scripts
        "歌词提供",
        "酷我歌词",
        "www.",
        "http://",
        "https://",
    };
    for (const auto& n : needles)
        if (raw.find(n) != std::string::npos) return true;
    return false;
}

bool isInstrumentalMarker(const std::string& t) {
    std::string s = lowerStr(trimStr(t));
    for (char& c : s) if (!std::isalnum((unsigned char)c)) c = ' ';
    return s.find("pure music") != std::string::npos ||
           s.find("instrumental") != std::string::npos ||
           s.find("纯音乐") != std::string::npos ||
           s.find("此歌曲为没有填词的纯音乐") != std::string::npos ||
           s.find("无歌词") != std::string::npos;
}

// Cache so choosing a hit does not require a second round-trip.
std::mutex cacheMu;
std::map<std::string, std::string> lrcCache;   // "provider:remoteId" -> LRC text
const std::size_t kCacheMax = 200;

void cachePut(const std::string& key, const std::string& val) {
    std::lock_guard<std::mutex> g(cacheMu);
    if (lrcCache.size() > kCacheMax) lrcCache.clear();
    lrcCache[key] = val;
}

bool cacheGet(const std::string& key, std::string& val) {
    std::lock_guard<std::mutex> g(cacheMu);
    auto it = lrcCache.find(key);
    if (it == lrcCache.end()) return false;
    val = it->second;
    return true;
}

// A short-lived HTTP GET used by every provider.
struct HttpResponse {
    bool ok = false;
    long status = 0;
    std::string body;
    std::string error;
};

// `retries` only covers transient failures (5xx / transport errors): lyric
// endpoints sit behind rate limiters and a single 503 is not unusual.
HttpResponse httpGet(const std::string& url, const std::string& referer,
                     long timeoutSec = 8, int retries = 1) {
    HttpResponse r;
    ExtractOptions opt;
    if (!referer.empty()) opt.referer = referer;
    opt.connectTimeoutSec = 5;
    opt.timeoutSec = timeoutSec;

    for (int attempt = 0; attempt <= retries; ++attempt) {
        if (attempt > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(400 * attempt));

        r = HttpResponse{};
        long curlCode = HttpClient::downloadToString(url, r.body, opt, &r.status);
        if (curlCode != CURLE_OK) {
            r.error = "network error (curl code " + std::to_string(curlCode) + ")";
            continue;                                   // transport: always retry
        }
        if (r.status >= 500 && r.status <= 599) {
            r.error = "HTTP " + std::to_string(r.status);
            continue;                                   // server side: worth a retry
        }
        if (r.status < 200 || r.status >= 300) {
            r.error = "HTTP " + std::to_string(r.status);
            return r;                                   // 4xx: decided, do not retry
        }
        r.ok = true;
        return r;
    }
    return r;
}

} // namespace

// ===========================================================================
// Lyric helpers
// ===========================================================================

std::string Lyric::plain() const {
    std::string out;
    for (const auto& l : lines) {
        if (!out.empty()) out += "\n";
        out += l.text;
    }
    return out;
}

bool Lyric::hasAnyText() const {
    for (const auto& l : lines)
        if (!trimStr(l.text).empty()) return true;
    return false;
}

static std::string fmtTime(double sec) {
    if (sec < 0) sec = 0;
    int total = (int)std::llround(sec * 100.0);
    int m = total / 6000;
    int s = (total % 6000) / 100;
    int cs = total % 100;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02d:%02d.%02d", m, s, cs);
    return std::string(buf);
}

std::string toLrc(const Lyric& l) {
    std::ostringstream os;
    if (!l.title.empty()) os << "[ti:" << l.title << "]\n";
    if (!l.artist.empty()) os << "[ar:" << l.artist << "]\n";
    if (!l.album.empty()) os << "[al:" << l.album << "]\n";
    if (l.offset != 0.0) {
        char b[32];
        std::snprintf(b, sizeof(b), "[offset:%+d]", (int)std::llround(l.offset * 1000));
        os << b << "\n";
    }
    for (const auto& line : l.lines)
        os << "[" << fmtTime(line.time) << "]" << line.text << "\n";
    return os.str();
}

std::string previewOf(const Lyric& l, std::size_t n) {
    std::string out;
    std::size_t got = 0;
    for (const auto& line : l.lines) {
        std::string t = trimStr(line.text);
        if (t.empty()) continue;
        if (!out.empty()) out += "  /  ";
        out += t;
        if (++got >= n) break;
    }
    if (out.size() > 120) out = out.substr(0, 120) + "…";
    return out;
}

// ===========================================================================
// parsing
// ===========================================================================

Lyric parseLrc(const std::string& text) {
    Lyric out;
    if (text.empty()) return out;

    // Strip a UTF-8 BOM if present.
    std::string src = text;
    if (src.size() >= 3 && (unsigned char)src[0] == 0xEF &&
        (unsigned char)src[1] == 0xBB && (unsigned char)src[2] == 0xBF)
        src = src.substr(3);

    // Normalise newlines.
    for (char& c : src)
        if (c == '\r') c = '\n';

    double globalOffset = 0.0;
    std::vector<std::pair<double, std::string>> raw;   // before offset applied

    std::size_t pos = 0;
    while (pos <= src.size()) {
        std::size_t nl = src.find('\n', pos);
        std::string line = (nl == std::string::npos) ? src.substr(pos)
                                                       : src.substr(pos, nl - pos);
        if (nl == std::string::npos) pos = src.size() + 1;
        else pos = nl + 1;

        if (line.empty()) continue;

        // Walk all leading [tag] groups.
        std::vector<double> stamps;
        std::size_t i = 0;
        bool sawMeta = false;
        while (i < line.size() && line[i] == '[') {
            std::size_t close = line.find(']', i);
            if (close == std::string::npos) break;
            std::string body = line.substr(i + 1, close - i - 1);

            if (body.size() >= 6 && lowerStr(body.substr(0, 6)) == "offset") {
                std::string v = trimStr(body.substr(6));
                if (!v.empty() && (v[0] == '+' || v[0] == '-')) v = v.substr(1);
                try { globalOffset = -std::stod(v) / 1000.0; } catch (...) {}
                i = close + 1;
                sawMeta = true;
                continue;
            }
            if (body.size() > 3 && body[2] == ':' &&
                (lowerStr(body.substr(0, 2)) == "ti" || lowerStr(body.substr(0, 2)) == "ar" ||
                 lowerStr(body.substr(0, 2)) == "al" || lowerStr(body.substr(0, 2)) == "by" ||
                 lowerStr(body.substr(0, 2)) == "re" || lowerStr(body.substr(0, 2)) == "ve" ||
                 lowerStr(body.substr(0, 2)) == "au" || lowerStr(body.substr(0, 2)) == "lr" ||
                 lowerStr(body.substr(0, 2)) == "co")) {
                std::string key = lowerStr(body.substr(0, 2));
                std::string val = trimStr(body.substr(3));
                if (key == "ti") out.title = val;
                else if (key == "ar") out.artist = val;
                else if (key == "al") out.album = val;
                i = close + 1;
                sawMeta = true;
                continue;
            }

            double t = 0.0;
            if (parseTimeTag(body, &t)) {
                stamps.push_back(t);
                i = close + 1;
                continue;
            }
            // Unknown tag: stop treating this as a lyric header.
            break;
        }

        std::string rest = (i < line.size()) ? line.substr(i) : std::string();
        if (sawMeta && stamps.empty()) continue;
        if (stamps.empty()) {
            // A line without any timestamp still belongs to the text; keep it
            // as a plain (unsynced) line so nothing gets lost.
            std::string t = trimStr(rest);
            if (!t.empty() && !isInstrumentalMarker(t) && !isBoilerplate(t)) raw.emplace_back(0.0, t);
            continue;
        }

        rest = trimStr(rest);
        if (rest.empty()) continue;
        if (isInstrumentalMarker(rest)) continue;
        if (isBoilerplate(rest)) continue;
        for (double t : stamps) raw.emplace_back(t, rest);
    }

    if (raw.empty()) return out;

    std::sort(raw.begin(), raw.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    out.found = true;
    out.offset = globalOffset;
    // Count real timestamps to decide if this is a karaoke-capable lyric.
    std::size_t timed = 0;
    for (const auto& r : raw) if (r.first > 0.0) ++timed;
    out.synced = timed >= 2;

    for (const auto& r : raw)
        out.lines.push_back(Line{r.first + globalOffset, r.second});

    // Drop consecutive duplicates that differ only in timestamp.
    std::vector<Line> dedup;
    for (const auto& l : out.lines) {
        if (!dedup.empty() && dedup.back().text == l.text &&
            std::fabs(dedup.back().time - l.time) < 0.01)
            continue;
        dedup.push_back(l);
    }
    out.lines = std::move(dedup);
    return out;
}

// ===========================================================================
// local files
// ===========================================================================

std::string lrcPathFor(const std::string& audioPath) {
    std::filesystem::path p = plat::u8path(audioPath);
    std::filesystem::path lrc = p.parent_path() / (p.stem().wstring() + L".lrc");
    return plat::pathToUtf8(lrc);
}

bool loadLocal(const std::string& audioPath, Lyric& out) {
    std::string p = lrcPathFor(audioPath);
    std::error_code ec;
    if (!std::filesystem::exists(plat::u8path(p), ec) || ec) return false;
    if (std::filesystem::is_directory(plat::u8path(p), ec) || ec) return false;

    FILE* f = plat::openFileRead(p);
    if (!f) return false;
    std::string text;
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
    fclose(f);

    Lyric parsed = parseLrc(text);
    if (!parsed.hasAnyText()) return false;
    parsed.localPath = p;
    parsed.source = "local";
    out = parsed;
    return true;
}

bool saveLocal(const std::string& audioPath, const std::string& lrc,
               std::string* writtenPath) {
    std::string p = lrcPathFor(audioPath);
    FILE* f = plat::openFileWrite(p);
    if (!f) return false;
    size_t w = fwrite(lrc.data(), 1, lrc.size(), f);
    fclose(f);
    if (w != lrc.size()) return false;
    if (writtenPath) *writtenPath = p;
    return true;
}

bool removeLocal(const std::string& audioPath) {
    std::string p = lrcPathFor(audioPath);
    std::error_code ec;
    bool existed = std::filesystem::exists(plat::u8path(p), ec);
    if (!existed || ec) return false;
    std::filesystem::remove(plat::u8path(p), ec);
    return !ec;
}

bool readEmbedded(const std::string& audioPath, Lyric& out) {
    AVFormatContext* fmt = nullptr;
    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "probesize", "1048576", 0);
    av_dict_set(&opts, "analyzeduration", "0", 0);

    int ret = avformat_open_input(&fmt, audioPath.c_str(), nullptr, &opts);
    av_dict_free(&opts);
    if (ret < 0 || !fmt) {
        if (fmt) avformat_close_input(&fmt);
        return false;
    }

    std::string text;
    AVDictionaryEntry* e = nullptr;
    while ((e = av_dict_get(fmt->metadata, "", e, AV_DICT_IGNORE_SUFFIX))) {
        std::string key = lowerStr(e->key);
        if (key == "lyrics" || key.rfind("lyrics-", 0) == 0 || key == "uslt" ||
            key == "unsynced lyrics" || key == "comment") {
            if (key == "comment") continue;
            if (e->value && *e->value) {
                text = e->value;
                break;
            }
        }
    }
    avformat_close_input(&fmt);

    if (text.empty()) return false;
    Lyric parsed = parseLrc(text);
    if (!parsed.hasAnyText()) return false;
    parsed.source = "embedded";
    out = parsed;
    return true;
}

// ===========================================================================
// URL encoding
// ===========================================================================

std::string urlEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += (char)c;
        else if (c == ' ') out += "%20";
        else {
            out += '%';
            out += hex[(c >> 4) & 0xF];
            out += hex[c & 0xF];
        }
    }
    return out;
}

// ===========================================================================
// providers
// ===========================================================================

namespace {

std::string jsonStr(const JsonValue* parent, const char* key, const std::string& def = "") {
    if (!parent) return def;
    const JsonValue* v = parent->find(key);
    if (!v || !v->isString()) return def;
    return v->strVal;
}

double jsonNum(const JsonValue* parent, const char* key, double def = 0.0) {
    if (!parent) return def;
    const JsonValue* v = parent->find(key);
    if (!v || !v->isNumber()) return def;
    return v->asDouble();
}

// Providers happily return the same song many times (karaoke edits, channel
// uploads, re-tagged rips). Two passes keep the list usable:
//   pass 1 – collapse results that describe the same track + artist
//   pass 2 – collapse results whose lyric text is byte-identical
// The highest scoring representative of each group survives.
std::vector<SearchHit> dedupHits(std::vector<SearchHit> hits, int limit) {
    auto collapse = [](std::vector<SearchHit>& v,
                       const std::function<std::string(const SearchHit&)>& keyFn) {
        std::map<std::string, SearchHit> best;
        std::vector<SearchHit> order;
        for (auto& h : v) {
            std::string k = keyFn(h);
            auto it = best.find(k);
            if (it == best.end()) { best[k] = h; order.push_back(h); }
            else if (h.score > it->second.score) {
                it->second = h;
                for (auto& o : order) if (keyFn(o) == k) { o = h; break; }
            }
        }
        v = order;
    };

    collapse(hits, [](const SearchHit& h) {
        return normKey(h.title) + "|" + normKey(h.artist);
    });
    collapse(hits, [](const SearchHit& h) { return "LRC::" + h.preview; });

    std::sort(hits.begin(), hits.end(),
              [](const SearchHit& a, const SearchHit& b) { return a.score > b.score; });
    if (limit > 0 && (int)hits.size() > limit) hits.resize(limit);
    return hits;
}

// ---------------------------------------------------------------- LRCLIB ---
// Public, key-free API. /api/search already embeds the full lyric text, so
// choosing a hit later never needs another request.
std::vector<SearchHit> searchLrclib(const Query& q, int limit, std::string* err) {
    std::vector<SearchHit> hits;
    std::string kw = q.title;
    if (!q.artist.empty()) kw += " " + q.artist;

    HttpResponse r = httpGet("https://lrclib.net/api/search?q=" + urlEncode(kw), "", 10);
    if (!r.ok) {
        if (err) *err += (err->empty() ? "" : "; ") + std::string("lrclib: ") + r.error;
        return hits;
    }

    JsonValue root;
    try { root = JsonParser::parse(r.body); } catch (const std::exception& ex) {
        if (err) *err += (err->empty() ? "" : "; ") + std::string("lrclib: bad JSON");
        return hits;
    }
    if (!root.isArray()) return hits;

    for (std::size_t i = 0; i < root.arr.size() && i < 25; ++i) {
        const JsonValue& v = root.arr[i];
        const JsonValue* instrumental = v.find("instrumental");
        if (instrumental && instrumental->isBool() && instrumental->boolVal) continue;

        std::string title = jsonStr(&v, "trackName");
        if (title.empty()) title = jsonStr(&v, "name");
        std::string artist = jsonStr(&v, "artistName");
        std::string album = jsonStr(&v, "albumName");

        std::string synced = jsonStr(&v, "syncedLyrics");
        std::string plain = jsonStr(&v, "plainLyrics");
        if (synced.empty() && plain.empty()) continue;

        Lyric probe = parseLrc(synced.empty() ? plain : synced);
        if (isInstrumentalMarker(probe.plain())) continue;

        SearchHit h;
        h.provider = "lrclib";
        h.remoteId = std::to_string((long long)v.asLong());
        {
            const JsonValue* idv = v.find("id");
            if (idv && idv->isNumber()) h.remoteId = idv->numStr;
        }
        h.title = title;
        h.artist = artist;
        h.album = album;
        h.duration = jsonNum(&v, "duration");
        h.synced = !synced.empty();
        h.preview = previewOf(probe, 2);
        h.score = scoreHit(q, title, artist, album, h.duration);
        if (h.synced) h.score += 4.0;   // prefer timed lyrics
        if (h.score < 18.0) continue;

        cachePut("lrclib:" + h.remoteId, synced.empty() ? plain : synced);
        hits.push_back(h);
    }
    return dedupHits(std::move(hits), limit);
}

// ------------------------------------------------------------- NetEase -----
// Old endpoints still work without cookies as long as a browser UA and the
// music.163.com Referer are present. Search returns metadata only, so each
// shortlisted song needs one extra call to grab the lyric body.
std::vector<SearchHit> searchNetease(const Query& q, int limit, std::string* err) {
    std::vector<SearchHit> hits;
    const std::string referer = "https://music.163.com/";

    std::string kw = q.title;
    if (!q.artist.empty()) kw += " " + q.artist;

    std::string url = "https://music.163.com/api/search/get?s=" + urlEncode(kw) +
                      "&type=1&offset=0&limit=10";
    HttpResponse r = httpGet(url, referer, 10);
    if (!r.ok) {
        if (err) *err += (err->empty() ? "" : "; ") + std::string("netease: ") + r.error;
        return hits;
    }

    JsonValue root;
    try { root = JsonParser::parse(r.body); } catch (...) {
        if (err) *err += (err->empty() ? "" : "; ") + std::string("netease: bad JSON");
        return hits;
    }

    const JsonValue* result = root.find("result");
    const JsonValue* songs = result ? result->find("songs") : nullptr;
    if (!songs || !songs->isArray()) {
        if (err && err->empty()) *err += "netease: empty search response";
        return hits;
    }
    if (songs->arr.empty()) {
        if (err && err->empty()) *err += "netease: no songs for query";
        return hits;
    }

    for (std::size_t i = 0; i < songs->arr.size() && i < 8; ++i) {
        const JsonValue& s = songs->arr[i];
        std::string title = jsonStr(&s, "name");
        if (title.empty()) continue;

        std::string artist;
        if (const JsonValue* arts = s.find("artists")) {
            for (std::size_t k = 0; k < arts->arr.size(); ++k) {
                std::string a = jsonStr(&arts->arr[k], "name");
                if (a.empty()) continue;
                if (!artist.empty()) artist += " / ";
                artist += a;
            }
        }
        std::string album;
        if (const JsonValue* al = s.find("album")) album = jsonStr(al, "name");

        const JsonValue* idv = s.find("id");
        if (!idv || !idv->isNumber()) continue;
        std::string id = idv->numStr;

        double durMs = jsonNum(&s, "duration");
        double dur = durMs > 0 ? durMs / 1000.0 : 0.0;

        // Score first so we can skip hopeless candidates without another call.
        double sc = scoreHit(q, title, artist, album, dur);
        if (sc < 18.0) continue;

        HttpResponse lr = httpGet(
            "https://music.163.com/api/song/lyric?id=" + id + "&lv=1&kv=1&tv=-1",
            referer, 8);
        if (!lr.ok) continue;

        JsonValue lj;
        try { lj = JsonParser::parse(lr.body); } catch (...) { continue; }

        std::string lrc;
        if (const JsonValue* lrcNode = lj.find("lrc"))
            lrc = jsonStr(lrcNode, "lyric");
        if (lrc.empty()) continue;

        Lyric probe = parseLrc(lrc);
        if (!probe.hasAnyText() || isInstrumentalMarker(probe.plain())) continue;

        SearchHit h;
        h.provider = "netease";
        h.remoteId = id;
        h.title = title;
        h.artist = artist;
        h.album = album;
        h.duration = dur;
        h.synced = probe.synced;
        h.preview = previewOf(probe, 2);
        h.score = sc;
        if (h.synced) h.score += 4.0;

        cachePut("netease:" + id, lrc);
        hits.push_back(h);
    }
    if (hits.empty() && err && err->empty())
        *err += "netease: every candidate was filtered out";
    return dedupHits(std::move(hits), limit);
}

} // namespace

std::vector<SearchHit> search(const Query& q, int maxHits, std::string* err) {
    if (q.title.empty()) return {};

    // Reserve room per provider so one chatty source cannot squeeze the other
    // out of the result list entirely.
    const int perProvider = std::max(3, (maxHits + 1) / 2);

    // Providers are independent — run them concurrently so a slow source does
    // not serialise behind a fast one.
    std::string errA, errB;
    auto fa = std::async(std::launch::async, [&] { return searchLrclib(q, perProvider, &errA); });
    auto fb = std::async(std::launch::async, [&] { return searchNetease(q, perProvider, &errB); });

    std::vector<SearchHit> all;
    try { for (auto& h : fa.get()) all.push_back(h); } catch (...) {}
    try { for (auto& h : fb.get()) all.push_back(h); } catch (...) {}

    if (err) {
        if (!errA.empty()) *err += errA;
        if (!errB.empty()) *err += (err->empty() ? "" : "; ") + errB;
    }

    // De-duplicate on provider + id.
    std::set<std::string> seen;
    std::vector<SearchHit> uniq;
    for (const auto& h : all) {
        std::string k = h.provider + ":" + h.remoteId;
        if (seen.count(k)) continue;
        seen.insert(k);
        uniq.push_back(h);
    }

    std::sort(uniq.begin(), uniq.end(),
              [](const SearchHit& a, const SearchHit& b) { return a.score > b.score; });
    if ((int)uniq.size() > maxHits) uniq.resize(maxHits);
    return uniq;
}

bool fetchByHit(const SearchHit& h, std::string& lrcOut, std::string* err) {
    std::string key = h.provider + ":" + h.remoteId;
    if (cacheGet(key, lrcOut) && !lrcOut.empty()) return true;

    if (h.provider == "netease") {
        HttpResponse r = httpGet(
            "https://music.163.com/api/song/lyric?id=" + h.remoteId + "&lv=1&kv=1&tv=-1",
            "https://music.163.com/", 8);
        if (!r.ok) {
            if (err) *err = "netease: " + r.error;
            return false;
        }
        JsonValue lj;
        try { lj = JsonParser::parse(r.body); } catch (...) {
            if (err) *err = "netease: bad JSON";
            return false;
        }
        std::string lrc;
        if (const JsonValue* n = lj.find("lrc")) lrc = jsonStr(n, "lyric");
        if (lrc.empty()) {
            if (err) *err = "netease: empty lyric";
            return false;
        }
        cachePut(key, lrc);
        lrcOut = lrc;
        return true;
    }

    if (h.provider == "lrclib") {
        HttpResponse r = httpGet("https://lrclib.net/api/get/" + urlEncode(h.remoteId), "", 10);
        if (!r.ok) {
            if (err) *err = "lrclib: " + r.error;
            return false;
        }
        JsonValue v;
        try { v = JsonParser::parse(r.body); } catch (...) {
            if (err) *err = "lrclib: bad JSON";
            return false;
        }
        std::string synced = jsonStr(&v, "syncedLyrics");
        std::string plain = jsonStr(&v, "plainLyrics");
        std::string lrc = synced.empty() ? plain : synced;
        if (lrc.empty()) {
            if (err) *err = "lrclib: empty lyric";
            return false;
        }
        cachePut(key, lrc);
        lrcOut = lrc;
        return true;
    }

    if (err) *err = "unknown provider: " + h.provider;
    return false;
}

} // namespace lyr
