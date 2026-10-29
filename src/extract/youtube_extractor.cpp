#include "youtube_extractor.h"
#include "../net/http_client.h"
#include "../common/json.h"
#include "../common/platform.h"

#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <filesystem>

namespace {

// --- small helpers ---------------------------------------------------------

std::string toLower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// Extract the 11-char YouTube video id from any supported URL shape:
//   https://www.youtube.com/watch?v=ID
//   https://youtu.be/ID
//   https://www.youtube.com/embed/ID
//   https://www.youtube.com/shorts/ID
//   https://www.youtube.com/v/ID
//   https://m.youtube.com/watch?v=ID
std::string extractVideoId(const std::string& url) {
    auto takeId = [](const std::string& s) -> std::string {
        std::string id;
        for (char c : s) {
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '_')
                id.push_back(c);
            else
                break;
        }
        return (id.size() == 11) ? id : std::string();
    };

    std::string low = toLower(url);
    // youtu.be/<id>
    size_t p = low.find("youtu.be/");
    if (p != std::string::npos)
        return takeId(url.substr(p + 9));
    // ?v=<id> (any youtube host)
    size_t q = low.find("v=");
    if (q != std::string::npos)
        return takeId(url.substr(q + 2));
    // /embed/<id>  /shorts/<id>  /v/<id>
    for (const char* seg : {"/embed/", "/shorts/", "/v/"}) {
        size_t s = low.find(seg);
        if (s != std::string::npos)
            return takeId(url.substr(s + std::string(seg).size()));
    }
    return "";
}

// Percent-decode a query component (needed for signatureCipher values).
std::string urlDecode(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        char c = in[i];
        if (c == '%' && i + 2 < in.size()) {
            auto hv = [](char h) -> int {
                if (h >= '0' && h <= '9') return h - '0';
                if (h >= 'a' && h <= 'f') return h - 'a' + 10;
                if (h >= 'A' && h <= 'F') return h - 'A' + 10;
                return -1;
            };
            int hi = hv(in[i + 1]), lo = hv(in[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back((char)((hi << 4) | lo));
                i += 2;
                continue;
            }
        } else if (c == '+') {
            out.push_back(' ');
            continue;
        }
        out.push_back(c);
    }
    return out;
}

// Parse a signatureCipher/cipher query string ("url=...&s=...&sp=...") and
// reconstruct the playable URL by appending the signature. This works for
// clients/formats that do NOT require the JS signature descramble step; for
// those that do, the resulting URL will 403 and we fall through to the next
// candidate client.
std::string decodeCipher(const std::string& cipher) {
    std::string baseUrl, sig, sp;
    size_t pos = 0;
    while (pos < cipher.size()) {
        size_t amp = cipher.find('&', pos);
        std::string kv = (amp == std::string::npos) ? cipher.substr(pos)
                                                    : cipher.substr(pos, amp - pos);
        size_t eq = kv.find('=');
        if (eq != std::string::npos) {
            std::string k = kv.substr(0, eq);
            std::string v = urlDecode(kv.substr(eq + 1));
            if (k == "url") baseUrl = v;
            else if (k == "s") sig = v;
            else if (k == "sp") sp = v;
        }
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
    if (baseUrl.empty()) return "";
    if (sig.empty()) return baseUrl;  // already a usable URL
    if (sp.empty()) sp = "signature";
    return baseUrl + (baseUrl.find('?') != std::string::npos ? '&' : '?') +
           sp + '=' + sig;
}

// Light sanitization so the title can be used as a file base name.
std::string sanitizeName(const std::string& in) {
    std::string out;
    for (char c : in) {
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|')
            out.push_back('_');
        else
            out.push_back(c);
    }
    // Trim trailing whitespace/dots (filesystems dislike "name.")
    while (!out.empty() &&
           (out.back() == ' ' || out.back() == '.' || out.back() == '\t'))
        out.pop_back();
    return out.empty() ? "youtube" : out;
}

// A client we try against the innertube player endpoint.
struct ClientSpec {
    std::string name;       // context.client.clientName
    std::string version;    // context.client.clientVersion
    std::string key;        // API key appended to the endpoint URL
    int numeric = 0;        // X-YouTube-Client-Name (0 = omit)
};

// Build the player request JSON body.
std::string buildPlayerBody(const std::string& videoId, const ClientSpec& c) {
    // Minimal but valid context. gl/hl are best-effort; YouTube tolerates them.
    return std::string("{\"context\":{\"client\":{\"clientName\":\"") + c.name +
           "\",\"clientVersion\":\"" + c.version +
           "\",\"hl\":\"en\",\"gl\":\"US\",\"utcOffsetMinutes\":0}},\"videoId\":\"" +
           videoId + "\",\"contentCheckOk\":true,\"racyCheckOk\":true}";
}

std::string playerEndpoint(const ClientSpec& c) {
    // Allow redirecting the whole endpoint (incl. path) for local testing.
    const char* override = std::getenv("YT_API_BASE");
    std::string base = override ? override : "https://www.youtube.com/youtubei/v1/player";
    return base + "?key=" + c.key;
}

// One adaptive audio stream candidate.
struct AudioCandidate {
    std::string url;
    long long bitrate = 0;
    bool directUrl = false;  // true if it came with a plain "url" (no cipher)
};

}  // namespace

bool YouTubeExtractor::supports(const std::string& url) const {
    std::string low = toLower(url);
    if (low.find("youtube.com") == std::string::npos &&
        low.find("youtu.be") == std::string::npos &&
        low.find("youtube-nocookie.com") == std::string::npos)
        return false;
    // Only claim real video URLs; let unrelated youtube.com pages fall through.
    return extractVideoId(url).size() == 11;
}

MediaSource YouTubeExtractor::extract(const std::string& url,
                                      const ExtractOptions& opt,
                                      const TempDir& tempDir) {
    std::string videoId = extractVideoId(url);
    if (videoId.empty())
        throw std::runtime_error("YouTubeExtractor: could not find a video id in the URL");
    LOG_INFO("YouTubeExtractor: videoId=" + videoId);

    // Clients, in priority order. TVHTML5 first because it returns direct
    // stream URLs without the signature / n-throttle requirement.
    std::vector<ClientSpec> clients = {
        {"TVHTML5", "", "", 95},
        {"WEB", "", "", 1},
        {"ANDROID", "", "", 3},
    };

    std::vector<std::pair<std::string, std::string>> postHeaders;
    ExtractOptions popt = opt;
    popt.referer = "https://www.youtube.com";
    popt.connectTimeoutSec = 20;
    popt.timeoutSec = 45;

    std::string lastErr = "YouTubeExtractor: no client produced a playable audio stream";

    for (const auto& client : clients) {
        postHeaders.clear();
        postHeaders.push_back({"Content-Type", "application/json"});
        postHeaders.push_back({"Origin", "https://www.youtube.com"});
        if (client.numeric != 0) {
            postHeaders.push_back(
                {"X-YouTube-Client-Name", std::to_string(client.numeric)});
            postHeaders.push_back({"X-YouTube-Client-Version", client.version});
        }

        std::string body = buildPlayerBody(videoId, client);
        std::string resp;
        long st = 0;
        long rc = HttpClient::postToString(playerEndpoint(client), body, popt,
                                           resp, &st, &postHeaders);
        if (rc != CURLE_OK) {
            lastErr = "YouTubeExtractor: player request failed for " + client.name +
                      " (curl " + std::to_string(rc) + ")";
            LOG_WARN(lastErr);
            continue;
        }
        if (st >= 400) {
            lastErr = "YouTubeExtractor: player HTTP " + std::to_string(st) +
                      " for client " + client.name;
            LOG_WARN(lastErr);
            continue;
        }

        JsonValue root;
        try {
            root = JsonParser::parse(resp);
        } catch (const std::exception& e) {
            lastErr = std::string("YouTubeExtractor: failed to parse player response (") +
                      e.what() + ")";
            LOG_WARN(lastErr);
            continue;
        }

        // Surface playability errors (age restriction, region lock, private...).
        const JsonValue* ps = root.find("playabilityStatus");
        if (ps) {
            const JsonValue* stt = ps->find("status");
            if (stt && stt->isString()) {
                std::string status = stt->asString();
                if (status != "OK" && status != "UNLOCKABLE") {
                    std::string reason = "unavailable";
                    const JsonValue* r = ps->find("reason");
                    if (r && r->isString()) reason = r->asString();
                    lastErr = "YouTubeExtractor: video is " + status + " (" + reason + ")";
                    LOG_WARN(lastErr);
                    continue;
                }
            }
        }

        const JsonValue* sd = root.find("streamingData");
        if (!sd || !sd->isObject()) {
            lastErr = "YouTubeExtractor: no streamingData for client " + client.name;
            LOG_WARN(lastErr);
            continue;
        }

        // Collect audio-only adaptive formats.
        std::vector<AudioCandidate> audio;
        const JsonValue* af = sd->find("adaptiveFormats");
        if (af && af->isArray()) {
            for (const auto& f : af->arr) {
                const JsonValue* mt = f.find("mimeType");
                if (!mt || !mt->isString()) continue;
                std::string mime = mt->asString();
                if (mime.rfind("audio/", 0) != 0) continue;  // audio only
                AudioCandidate cand;
                const JsonValue* br = f.find("bitrate");
                if (br && br->isNumber()) cand.bitrate = br->asLong();
                const JsonValue* u = f.find("url");
                if (u && u->isString() && !u->asString().empty()) {
                    cand.url = u->asString();
                    cand.directUrl = true;
                } else {
                    const JsonValue* sc = f.find("signatureCipher");
                    if (!sc || !sc->isString()) sc = f.find("cipher");
                    if (sc && sc->isString()) {
                        cand.url = decodeCipher(sc->asString());
                        cand.directUrl = false;
                    }
                }
                if (!cand.url.empty()) audio.push_back(std::move(cand));
            }
        }

        if (audio.empty()) {
            lastErr = "YouTubeExtractor: no audio-only formats for client " + client.name;
            LOG_WARN(lastErr);
            continue;
        }

        // Prefer direct-URL formats, then highest bitrate.
        std::sort(audio.begin(), audio.end(),
                  [](const AudioCandidate& a, const AudioCandidate& b) {
                      if (a.directUrl != b.directUrl) return a.directUrl > b.directUrl;
                      return a.bitrate > b.bitrate;
                  });

        // Pick a sensible output container based on the chosen format's mime.
        std::string title = "youtube-" + videoId;
        const JsonValue* vd = root.find("videoDetails");
        if (vd && vd->isObject()) {
            const JsonValue* t = vd->find("title");
            if (t && t->isString() && !t->asString().empty())
                title = sanitizeName(t->asString());
        }

        std::string bestMime;  // remembered for extension selection
        for (const auto& cand : audio) {
            // Choose extension from the first audio format's mime (best guess).
            if (bestMime.empty()) bestMime = cand.url;

            std::string outName = "yt_audio";
            std::string lowerUrl = toLower(cand.url);
            if (lowerUrl.find("webm") != std::string::npos ||
                lowerUrl.find("opus") != std::string::npos)
                outName += ".weba";
            else
                outName += ".m4a";
            std::string outPath = tempDir.join(outName);

            long localSt = 0;
            long r = HttpClient::downloadToFile(cand.url, outPath, popt, &localSt);
            bool ok = (r == CURLE_OK && localSt < 400 &&
                       std::filesystem::file_size(plat::u8path(outPath)) > 0);
            if (!ok) {
                LOG_WARN("YouTubeExtractor: audio download failed (" + client.name +
                         ", curl " + std::to_string(r) + ", http " +
                         std::to_string(localSt) + "); trying next candidate");
                continue;
            }
            LOG_INFO("YouTubeExtractor: downloaded audio via " + client.name +
                     " (bitrate " + std::to_string(cand.bitrate) + ")");
            MediaSource src;
            src.mode = MediaSource::Mode::FILE;
            src.filePath = outPath;
            src.suggestedName = title;
            return src;
        }
        lastErr = "YouTubeExtractor: all audio URLs failed for client " + client.name +
                  " (likely throttled / signature-protected)";
        LOG_WARN(lastErr);
    }

    throw std::runtime_error(lastErr);
}
