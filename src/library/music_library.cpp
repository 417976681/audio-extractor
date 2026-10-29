#include "music_library.h"
#include "common/common.h"
#include "common/platform.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <functional>

#ifdef _WIN32
#include <sys/stat.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/dict.h>
#include <libavutil/frame.h>
#include <libavutil/rational.h>
}

namespace {

bool hasAudioExtension(const std::string& p) {
    static const char* kExts[] = {
        "mp3", "wav", "flac", "m4a", "aac", "ogg", "oga", "opus",
        "mka", "wma", "aiff", "aif", "ape", "mp2", "ac3", "alac"
    };
    std::string ext = lowerExtension(p);
    for (const char* e : kExts) if (ext == e) return true;
    return false;
}

std::string tagOf(AVDictionary* md, const char* key, const char* alt = nullptr) {
    if (!md) return "";
    AVDictionaryEntry* e = av_dict_get(md, key, nullptr, 0);
    if (!e && alt) e = av_dict_get(md, alt, nullptr, 0);
    if (!e) return "";
    std::string v = e->value;
    // Trim whitespace.
    size_t a = v.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = v.find_last_not_of(" \t\r\n");
    return v.substr(a, b - a + 1);
}

// Unix-time seconds of a file's last write.
//
// NOTE: std::filesystem::last_write_time() is NOT portable here — MinGW's
// file_clock uses the Windows FILETIME epoch, so casting its raw count yields
// a nonsensical negative number. Go straight to the platform stat() instead.
int64_t fileMtime(const std::filesystem::path& p) {
#ifdef _WIN32
    struct _stat64 st;
    if (_wstat64(p.wstring().c_str(), &st) != 0) return 0;
    int64_t t = (int64_t)st.st_mtime;
#else
    struct stat st;
    if (::stat(p.c_str(), &st) != 0) return 0;
    int64_t t = (int64_t)st.st_mtime;
#endif
    // Sanity guard: reject anything outside 1970..2200 so a bad read can never
    // poison "recently added" ordering.
    if (t < 0 || t > 7258118400LL) return 0;
    return t;
}

}  // namespace

// ---------------------------------------------------------------------------
// JSON helpers
// ---------------------------------------------------------------------------
std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                // Keep UTF-8 bytes verbatim; escape only C0 control characters.
                if (c < 0x20) {
                    static const char* hex = "0123456789abcdef";
                    out += "\\u00";
                    out += hex[(c >> 4) & 0xF];
                    out += hex[c & 0xF];
                } else {
                    out += (char)c;
                }
        }
    }
    return out;
}

std::string trackToJson(const TrackMeta& t) {
    // Does a lyric file ("song.lrc") sit next to the audio file? The UI needs
    // this to warn that deleting the track also removes its lyrics.
    std::filesystem::path lrc = plat::u8path(t.path);
    lrc.replace_extension(".lrc");
    std::error_code lec;
    const bool hasLyric = std::filesystem::exists(lrc, lec);

    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  ",\"duration\":%.3f,\"size\":%lld,\"bitrate\":%d,"
                  "\"sampleRate\":%d,\"channels\":%d,\"year\":%d,\"mtime\":%lld,"
                  "\"hasCover\":%s,\"hasLyric\":%s",
                  t.duration, (long long)t.size, t.bitrateKbps, t.sampleRate,
                  t.channels, t.year, (long long)t.mtime,
                  t.hasCover ? "true" : "false", hasLyric ? "true" : "false");
    return std::string("{")
         + "\"id\":\""     + jsonEscape(t.id)       + "\""
         + ",\"title\":\"" + jsonEscape(t.title)    + "\""
         + ",\"artist\":\"" + jsonEscape(t.artist)  + "\""
         + ",\"album\":\"" + jsonEscape(t.album)    + "\""
         + ",\"genre\":\"" + jsonEscape(t.genre)    + "\""
         + ",\"codec\":\"" + jsonEscape(t.codec)    + "\""
         + ",\"fileName\":\"" + jsonEscape(t.fileName) + "\""
         + buf
         + "}";
}

// ---------------------------------------------------------------------------
// MusicLibrary
// ---------------------------------------------------------------------------
std::string MusicLibrary::makeId(const std::string& absPath) {
    // FNV-1a 64 over the UTF-8 bytes of the absolute path.
    uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : absPath) {
        h ^= (uint64_t)c;
        h *= 1099511628211ULL;
    }
    static const char* hex = "0123456789abcdef";
    std::string out;
    for (int i = 15; i >= 0; --i) out += hex[(h >> (i * 4)) & 0xF];
    return out;
}

std::string MusicLibrary::sanitizeFileName(const std::string& name) {
    // Characters that are illegal in Windows/POSIX file names.
    static const std::string kBad = "\\/:*?\"<>|";
    std::string out;
    for (unsigned char c : name) {
        if (c < 0x20 || kBad.find((char)c) != std::string::npos) out += '_';
        else out += (char)c;
    }
    // Trim trailing dots/spaces (Windows quirk).
    while (!out.empty() && (out.back() == '.' || out.back() == ' ')) out.pop_back();
    if (out.empty()) out = "track";
    if (out.size() > 120) out = out.substr(0, 120);
    return out;
}

void MusicLibrary::setDirectories(const std::vector<std::string>& dirs) {
    std::lock_guard<std::mutex> lk(mu_);
    dirs_ = dirs;
    scanned_ = false;
}

bool MusicLibrary::addDirectory(const std::string& dir) {
    std::lock_guard<std::mutex> lk(mu_);
    std::error_code ec;
    auto p = plat::u8path(dir);
    if (!std::filesystem::is_directory(p, ec)) return false;
    std::string canon = plat::pathToUtf8(std::filesystem::canonical(p, ec));
    if (canon.empty()) canon = dir;
    if (std::find(dirs_.begin(), dirs_.end(), canon) != dirs_.end()) return true;
    dirs_.push_back(canon);
    scanned_ = false;
    return true;
}

bool MusicLibrary::removeDirectory(const std::string& dir) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = std::find(dirs_.begin(), dirs_.end(), dir);
    if (it == dirs_.end()) return false;
    dirs_.erase(it);
    scanned_ = false;
    return true;
}

bool MusicLibrary::readFileMeta(const std::string& path, TrackMeta& out,
                                std::vector<uint8_t>* coverOut,
                                std::string* coverMime) {
    const std::string abs = plat::pathToUtf8(
        std::filesystem::absolute(plat::u8path(path)));
    out = TrackMeta{};
    out.path = abs;
    out.id = makeId(abs);
    out.fileName = plat::pathToUtf8(plat::u8path(path).filename());
    out.title = plat::pathToUtf8(plat::u8path(path).stem());

    std::error_code ec;
    out.mtime = fileMtime(plat::u8path(path));
    out.size = (int64_t)std::filesystem::file_size(plat::u8path(path), ec);

    // FFmpeg needs a UTF-8 filename; on Windows this may not round-trip through
    // its own file protocol, so hand it an opened-to-wide shareable name via
    // avformat_open_input's filename slot (av_cwd_abs is fine for most builds).
    AVFormatContext* fmt = nullptr;
    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "probesize", "1048576", 0);   // keep probing fast
    av_dict_set(&opts, "analyzeduration", "0", 0);
    if (avformat_open_input(&fmt, path.c_str(), nullptr, &opts) != 0) {
        av_dict_free(&opts);
        // Even without a container we can guess CBR length from the file size.
        std::string ext = lowerExtension(path);
        if (out.size > 0 && (ext == "mp3" || ext == "mp2")) {
            // Assume a plausible 128 kbps floor; better than showing 0:00.
            out.duration = (double)out.size * 8.0 / (128000.0);
        }
        out.codec = ext;
        return true;   // the file exists, we just lack metadata
    }
    if (opts) av_dict_free(&opts);

    if (avformat_find_stream_info(fmt, nullptr) >= 0) {
        int aIdx = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
        if (aIdx >= 0) {
            AVStream* st = fmt->streams[aIdx];
#if LIBAVUTIL_VERSION_MAJOR >= 57
            const AVCodecParameters* cp = st->codecpar;
            out.codec      = avcodec_get_name(cp->codec_id);
            out.sampleRate = cp->sample_rate;
            out.channels   = cp->ch_layout.nb_channels;
#else
            out.codec      = avcodec_get_name(st->codecpar->codec_id);
            out.sampleRate = st->codecpar->sample_rate;
            out.channels   = st->codecpar->channels;
#endif
            int64_t br = fmt->bit_rate;
            if (br <= 0) br = st->codecpar->bit_rate;
            out.bitrateKbps = (int)((br + 500) / 1000);

            double dur = 0;
            if (st->duration > 0 && st->time_base.den)
                dur = (double)st->duration * av_q2d(st->time_base);
            else if (fmt->duration > 0)
                dur = (double)fmt->duration / (double)AV_TIME_BASE;
            out.duration = dur;
        }
        // Embedded cover art (ID3 APIC / MP4 covr / FLAC picture block).
        for (unsigned i = 0; i < fmt->nb_streams; ++i) {
            AVStream* st = fmt->streams[i];
            if (st->codecpar->codec_type != AVMEDIA_TYPE_VIDEO) continue;
            if (!(st->disposition & AV_DISPOSITION_ATTACHED_PIC)) continue;

            // The actual image bytes live in the AVPacket attached to the
            // stream; codecpar->extradata only carries them for some demuxers.
            const uint8_t* data = nullptr;
            size_t size = 0;
            if (st->attached_pic.size > 0 && st->attached_pic.data) {
                data = st->attached_pic.data;
                size = (size_t)st->attached_pic.size;
            } else if (st->codecpar->extradata_size > 0 && st->codecpar->extradata) {
                data = st->codecpar->extradata;
                size = (size_t)st->codecpar->extradata_size;
            }
            if (!data || size == 0) break;

            out.hasCover = true;
            if (coverOut) {
                coverOut->assign(data, data + size);
                std::string m = "image/jpeg";
                if (size >= 8 && data[0] == 0x89 && data[1] == 'P' &&
                    data[2] == 'N' && data[3] == 'G') m = "image/png";
                else if (size >= 12 && data[0] == 'R' && data[1] == 'I' &&
                         data[2] == 'F' && data[3] == 'F') m = "image/webp";
                if (coverMime) *coverMime = m;
            }
            break;
        }
    }

    out.title  = tagOf(fmt->metadata, "title");
    out.artist = tagOf(fmt->metadata, "artist", "album_artist");
    out.album  = tagOf(fmt->metadata, "album");
    out.genre  = tagOf(fmt->metadata, "genre");
    std::string date = tagOf(fmt->metadata, "date", "year");
    if (!date.empty()) {
        try { out.year = std::stoi(date.substr(0, 4)); } catch (...) {}
    }
    if (out.title.empty()) out.title = plat::pathToUtf8(plat::u8path(path).stem());
    if (out.duration <= 0 && out.bitrateKbps > 0 && out.size > 0)
        out.duration = (double)out.size * 8.0 / (out.bitrateKbps * 1000.0);

    avformat_close_input(&fmt);
    return true;
}

std::vector<TrackMeta> MusicLibrary::scan(bool force) {
    std::lock_guard<std::mutex> lk(mu_);
    if (scanned_ && !force) return cache_;

    std::vector<TrackMeta> result;
    for (const auto& dir : dirs_) {
        std::error_code ec;
        auto root = plat::u8path(dir);
        if (!std::filesystem::is_directory(root, ec)) continue;
        auto it = std::filesystem::recursive_directory_iterator(root, ec);
        auto end = std::filesystem::recursive_directory_iterator();
        while (it != end) {
            if (ec) { it.increment(ec); continue; }
            std::filesystem::path p;
            bool isFile = false;
            {
                std::error_code ec2;
                isFile = it->is_regular_file(ec2);
                if (!ec2) p = it->path();
            }
            if (isFile) {
                std::string ps = plat::pathToUtf8(p);
                if (hasAudioExtension(ps)) {
                    TrackMeta m;
                    std::vector<uint8_t> cov;
                    std::string mime;
                    if (readFileMeta(ps, m, &cov, &mime)) {
                        if (m.hasCover && !cov.empty())
                            coverCache_[m.id] = std::make_pair(mime, std::move(cov));
                        else
                            coverCache_.erase(m.id);
                        result.push_back(std::move(m));
                    }
                }
            }
            it.increment(ec);
        }
    }

    // Most recently touched first — freshly extracted tracks show up on top.
    std::sort(result.begin(), result.end(), [](const TrackMeta& a, const TrackMeta& b) {
        if (a.mtime != b.mtime) return a.mtime > b.mtime;
        return a.title < b.title;
    });

    cache_ = std::move(result);
    scanned_ = true;
    return cache_;
}

bool MusicLibrary::find(const std::string& id, TrackMeta& out) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto c = const_cast<const std::vector<TrackMeta>&>(cache_);
        for (const auto& t : c) if (t.id == id) { out = t; return true; }
    }
    if (scanned_) return false;
    auto all = scan(false);
    for (const auto& t : all) if (t.id == id) { out = t; return true; }
    return false;
}

bool MusicLibrary::cover(const std::string& id, std::vector<uint8_t>& out, std::string& mime) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = coverCache_.find(id);
    if (it == coverCache_.end()) return false;
    mime = it->second.first;
    out = it->second.second;
    return !out.empty();
}
