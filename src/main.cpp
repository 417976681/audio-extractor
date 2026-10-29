#include "common/common.h"
#include "common/platform.h"
#include "net/http_client.h"
#include "extract/extractor.h"
#include "extract/registry.h"
#include "audio/audio_extractor.h"

#include <iostream>
#include <string>
#include <cstring>
#include <vector>
#include <filesystem>

static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

static void printUsage(const char* prog) {
    std::cout
        << "Audio Extractor - extract audio from video websites (C++)\n"
        << "Usage:\n"
        << "  " << prog << " [options] <video-url>\n"
        << "  " << prog << " [options] -l <url-list.txt>\n\n"
        << "Options:\n"
        << "  -o, --output FILE     Output file (default: output.mp3)\n"
        << "                        Parent directories are created automatically.\n"
        << "                        Both \"--output FILE\" and \"--output=FILE\" work.\n"
        << "  -f, --format FMT      mp3 | wav (default: inferred from extension, else mp3)\n"
        << "  -b, --bitrate KBPS    MP3 bitrate in kbps (default: 192)\n"
        << "  -r, --rate HZ         Output sample rate (default: 44100)\n"
        << "  -c, --channels N      Output channels (default: 2)\n"
        << "  -l, --list FILE       Batch mode: one <url> [output] per line (# comments)\n"
        << "  -u, --user-agent UA   Custom User-Agent\n"
        << "      --referer URL     Referer header\n"
        << "      --cookie STR      Cookie header\n"
        << "  -h, --help            Show this help\n\n"
        << "Notes:\n"
        << "  - Full UTF-8 support: Chinese file names and paths work everywhere.\n"
        << "  - Values containing spaces SHOULD be quoted: -o \"我的音乐/xx.mp3\"\n"
        << "    (If you forget the quotes, the tool usually recovers automatically.)\n\n"
        << "Examples:\n"
        << "  " << prog << " \"https://example.com/video.mp4\" -o song.mp3\n"
        << "  " << prog << " \"https://example.com/live/playlist.m3u8\" -f wav -o rec.wav\n"
        << "  " << prog << " \"https://example.com/v.m3u8\" --referer \"https://site/\" -b 320\n"
        << "  " << prog << " \"https://b23.tv/AbCdEf\" -o bili.mp3 -b 320\n"
        << "  " << prog << " \"https://b23.tv/AbCdEf\" -o \"音乐/好听的歌 - 翻唱.mp3\" -b 320\n"
        << "  " << prog << " -l urls.txt -b 320\n\n"
        << "url list file format (UTF-8, one task per line):\n"
        << "  <url>                 # auto-named: <output>_1.mp3, _2.mp3 ...\n"
        << "  <url> <output-file>   # output may contain spaces, no quotes needed\n"
        << "  # comment line        # lines starting with # are skipped\n";
}

// Split "--opt=value" / "-o=value" into (name, value). Only recognizes real options
// so that URLs containing '=' are never mangled.
static bool splitOptValue(const std::string& tok, std::string& name, std::string& val) {
    static const char* kKnown[] = {
        "-o", "--output", "-f", "--format", "-b", "--bitrate", "-r", "--rate",
        "-c", "--channels", "-l", "--list", "-u", "--user-agent",
        "--referer", "--cookie"
    };
    if (tok.size() < 2 || tok[0] != '-') return false;
    size_t eq = tok.find('=');
    if (eq == std::string::npos || eq == 0) return false;
    name = tok.substr(0, eq);
    val = tok.substr(eq + 1);
    for (const char* k : kKnown) if (name == k) return true;
    return false;
}

// A token that introduces an option, e.g. "-b" or "--cookie". A lone "-" is not
// an option: it is a normal character that may appear inside file names.
static bool isOptionToken(const std::string& s) {
    return s.size() > 1 && s[0] == '-';
}

static int parseInt(const std::string& name, const std::string& s, int fallback) {
    try { return std::stoi(s); }
    catch (...) {
        std::cerr << "Invalid number for " << name << ": " << s
                  << " (using default " << fallback << ")\n";
        return fallback;
    }
}

static std::string ensureExtension(const std::string& path, const std::string& ext) {
    std::string e = lowerExtension(path);
    if (e == ext) return path;
    return plat::withSuffix(path, "." + ext);
}

// Extract audio from a single URL into `output`. Returns 0 on success.
static int processUrl(const std::string& url,
                      std::string output,
                      AudioFormat fmt,
                      const std::string& fmtExt,
                      int bitrate, int rate, int channels,
                      const ExtractOptions& extractOpt) {
    output = ensureExtension(output, fmtExt);

    // Make sure the destination directory exists (supports nested / Chinese paths).
    {
        std::filesystem::path op = plat::u8path(output);
        std::filesystem::path parent = op.parent_path();
        if (!parent.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(parent, ec);
            if (!std::filesystem::is_directory(parent, ec)) {
                std::cerr << "\nCannot create output directory: "
                          << plat::pathToUtf8(parent) << "\n";
                return 1;
            }
        }
    }

    int lastPct = -1;
    ExtractOptions opt = extractOpt;  // copy so each job has its own progress state
    opt.progress = [&](double frac) {
        int pct = (frac < 0) ? -1 : (int)(frac * 100);
        if (pct != lastPct) {
            lastPct = pct;
            if (pct < 0) std::cerr << ".";
            else std::cerr << "\rProgress: " << pct << "%" << std::flush;
        }
    };

    int rc = 0;
    try {
        IExtractor* ex = ExtractorRegistry::select(url);
        if (!ex) {
            std::cerr << "\n[ERROR] No extractor supports: " << url << "\n";
            return 1;
        }
        LOG_INFO(std::string("Using extractor: ") + ex->name() + "  (" + url + ")");
        TempDir temp;
        MediaSource src = ex->extract(url, opt, temp);

        AudioOptions aopt;
        aopt.format = fmt;
        aopt.sampleRate = rate;
        aopt.channels = channels;
        aopt.bitrateKbps = bitrate;
        aopt.outputPath = output;
        aopt.progress = opt.progress;

        LOG_INFO("Extracting audio -> " + output);
        int er = extractAudio(src, aopt);
        if (er != 0) {
            std::cerr << "\nAudio extraction failed (code " << er << ")\n";
            rc = 1;
        } else {
            std::cerr << "\rProgress: 100%\n";
            std::error_code ec;
            auto sz = std::filesystem::file_size(plat::u8path(output), ec);
            LOG_INFO("Done: " + output + (ec ? "" : " (" + std::to_string(sz) + " bytes)"));
        }
    } catch (const std::exception& e) {
        std::cerr << "\nError: " << e.what() << "\n";
        rc = 1;
    }
    return rc;
}

int main(int argc, char** argv) {
    // On Windows this yields proper UTF-8 arguments (incl. Chinese paths);
    // on POSIX it simply wraps argv.
    std::vector<std::string> args = plat::getCommandLineArgs(argc, argv);

    std::string url;
    std::string output = "output.mp3";
    std::string listFile;
    std::string formatStr;          // empty => infer
    int bitrate = 192;
    int rate = 44100;
    int channels = 2;
    ExtractOptions extractOpt;

    // Expand "--opt=value" into two separate tokens.
    std::vector<std::string> toks;
    toks.reserve(args.size() + 4);
    for (const auto& a : args) {
        std::string n, v;
        if (splitOptValue(a, n, v)) { toks.push_back(n); toks.push_back(v); }
        else toks.push_back(a);
    }

    // A value that was unquoted and contains spaces arrives as several tokens
    // (e.g.  -o musices/福星XianFu - 能治愈你的不是时间.mp3 ). We recover by
    // appending those extra tokens to the value that was just parsed.
    std::string* lastValue = nullptr;
    const char*  lastOpt   = "";
    bool warnedMerge = false;

    for (size_t i = 1; i < toks.size(); ++i) {
        std::string a = toks[i];
        auto needVal = [&](const char* name) -> std::string {
            if (i + 1 >= toks.size()) { std::cerr << "Missing value for " << name << "\n"; exit(2); }
            return toks[++i];
        };
        auto remember = [&](const char* name, std::string* target) { lastOpt = name; lastValue = target; };

        if (a == "-h" || a == "--help") { printUsage(args[0].c_str()); return 0; }
        else if (a == "-o" || a == "--output") { output = needVal("--output");     remember("--output", &output); }
        else if (a == "-l" || a == "--list")   { listFile = needVal("--list");     remember("--list", &listFile); }
        else if (a == "-f" || a == "--format") { formatStr = needVal("--format");  remember("--format", &formatStr); }
        else if (a == "-u" || a == "--user-agent") { extractOpt.userAgent = needVal("--user-agent"); remember("--user-agent", &extractOpt.userAgent); }
        else if (a == "--referer") { extractOpt.referer = needVal("--referer");    remember("--referer", &extractOpt.referer); }
        else if (a == "--cookie")  { extractOpt.cookie = needVal("--cookie");      remember("--cookie", &extractOpt.cookie); }
        else if (a == "-b" || a == "--bitrate") { bitrate  = parseInt("--bitrate",  needVal("--bitrate"),  bitrate);  lastValue = nullptr; }
        else if (a == "-r" || a == "--rate")    { rate     = parseInt("--rate",     needVal("--rate"),     rate);     lastValue = nullptr; }
        else if (a == "-c" || a == "--channels"){ channels = parseInt("--channels", needVal("--channels"), channels); lastValue = nullptr; }
        else if (isOptionToken(a)) {
            std::cerr << "Unknown option: " << a << "\n";
            printUsage(args[0].c_str()); return 2;
        } else {
            // Not an option -> either the URL, or a stray piece of an unquoted value.
            std::string* target = nullptr;
            if (lastValue) target = lastValue;
            else if (!url.empty()) { target = &url; lastOpt = "<url>"; }
            else { url = a; remember("<url>", &url); continue; }

            *target += " " + a;
            if (!warnedMerge) {
                warnedMerge = true;
                std::cerr << "[WARN] Value containing spaces was not quoted; recovering.\n"
                          << "       Tip: write -o \"my dir/my song.mp3\" instead.\n";
            }
        }
    }

    if (warnedMerge) std::cerr << "[WARN] Recovered output path: \"" << output << "\"\n";

    // Determine format.
    AudioFormat fmt = AudioFormat::MP3;
    if (!formatStr.empty()) {
        if (formatStr == "mp3") fmt = AudioFormat::MP3;
        else if (formatStr == "wav") fmt = AudioFormat::WAV;
        else { std::cerr << "Unknown format: " << formatStr << "\n"; return 2; }
    } else {
        std::string e = lowerExtension(output);
        if (e == "wav") fmt = AudioFormat::WAV;
        else if (e == "mp3") fmt = AudioFormat::MP3;
        // else default MP3 (already set)
    }
    std::string fmtExt = (fmt == AudioFormat::WAV) ? "wav" : "mp3";

    if (!HttpClient::init()) {
        std::cerr << "Failed to initialize libcurl\n";
        return 1;
    }
    ExtractorRegistry::initDefaults();

    int rc = 0;

    if (!listFile.empty()) {
        // ---- Batch mode -----------------------------------------------------
        FILE* inF = plat::openFileRead(listFile);
        if (!inF) {
            std::cerr << "Error: cannot open list file: " << listFile << "\n";
            HttpClient::cleanup();
            return 2;
        }
        // Read the whole file as UTF-8 and split into lines (handles CRLF).
        std::string content;
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), inF)) > 0) content.append(buf, n);
        fclose(inF);

        // Strip a UTF-8 BOM (Windows editors like to add one).
        if (content.size() >= 3 && (unsigned char)content[0] == 0xEF &&
            (unsigned char)content[1] == 0xBB && (unsigned char)content[2] == 0xBF) {
            content.erase(0, 3);
        }

        int idx = 0, ok = 0, fail = 0;
        size_t pos = 0;
        while (pos <= content.size()) {
            size_t nl = content.find('\n', pos);
            if (nl == std::string::npos) nl = content.size();
            std::string line = content.substr(pos, nl - pos);
            pos = nl + 1;
            if (!line.empty() && line.back() == '\r') line.pop_back();

            line = trim(line);
            if (line.empty() || line[0] == '#') continue;
            // Optional "URL OUTPUT" (first whitespace-delimited token is the URL).
            std::string u, out;
            size_t sp = line.find_first_of(" \t");
            if (sp == std::string::npos) u = line;
            else { u = line.substr(0, sp); out = trim(line.substr(sp + 1)); }
            if (u.empty()) continue;

            idx++;
            if (out.empty()) {
                out = plat::withSuffix(output, "_" + std::to_string(idx));
            }
            std::cerr << "\n[" << idx << "] " << u << "\n";
            int r = processUrl(u, out, fmt, fmtExt, bitrate, rate, channels, extractOpt);
            if (r == 0) ok++; else fail++;
        }
        LOG_INFO("Batch complete: " + std::to_string(ok) + " succeeded, " +
                 std::to_string(fail) + " failed (of " + std::to_string(idx) + " total)");
        rc = fail ? 1 : 0;
    } else {
        // ---- Single URL mode ------------------------------------------------
        if (url.empty()) {
            std::cerr << "Error: no URL supplied.\n\n";
            printUsage(args[0].c_str());
            HttpClient::cleanup();
            return 2;
        }
        rc = processUrl(url, output, fmt, fmtExt, bitrate, rate, channels, extractOpt);
    }

    HttpClient::cleanup();
    return rc;
}
