#pragma once

#include "extractor.h"

// Handles a direct media URL (e.g. https://.../video.mp4 or .webm).
// Downloads the whole file via libcurl, then hands the local path to the
// audio extractor. Most "video website" embeds expose a direct media URL;
// this is the universal fallback extractor.
class DirectExtractor : public IExtractor {
public:
    bool supports(const std::string& url) const override;
    const char* name() const override { return "direct"; }
    MediaSource extract(const std::string& url,
                        const ExtractOptions& opt,
                        const TempDir& tempDir) override;
};
