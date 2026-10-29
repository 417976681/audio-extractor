#pragma once

#include <string>
#include "common/common.h"

// Result of an extraction step: where the audio extractor can find the media
// on the local filesystem. Everything lives inside the extractor's TempDir,
// which the caller must keep alive until audio extraction is finished.
struct MediaSource {
    enum class Mode { FILE, CONCAT };

    Mode mode = Mode::FILE;

    // For Mode::FILE: a single local media file.
    std::string filePath;

    // For Mode::CONCAT: a concat demuxer list file (absolute paths).
    std::string concatListPath;

    // Optional hint in seconds (sum of HLS segment durations, etc.).
    double durationHint = 0.0;

    // Optional human-friendly base name (without extension) reported by site
    // extractors that know the video title. Callers may use it to name the
    // output file; an empty string means "no suggestion".
    std::string suggestedName;
};

// Interface implemented by every site/format extractor. Add a new site by
// implementing this and registering it in the registry.
class IExtractor {
public:
    virtual ~IExtractor() = default;

    // True if this extractor can handle the given URL.
    virtual bool supports(const std::string& url) const = 0;

    // Human-readable name (e.g. "direct", "hls").
    virtual const char* name() const = 0;

    // Produce a local MediaSource for the audio extractor to process.
    // `tempDir` is owned by the caller and lives until extraction completes.
    virtual MediaSource extract(const std::string& url,
                                const ExtractOptions& opt,
                                const TempDir& tempDir) = 0;
};

// Pick the first registered extractor that supports `url`.
class ExtractorRegistry {
public:
    static void registerExtractor(IExtractor* ex);
    static IExtractor* select(const std::string& url);
    static void initDefaults();
};
