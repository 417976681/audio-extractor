#pragma once

#include "extractor.h"

// Site-specific extractor for Bilibili (bilibili.com video pages).
//
// Bilibili does not expose a plain media URL on its watch pages; the audio is
// delivered as a DASH stream behind an API that requires a WBI signature. This
// extractor performs the standard flow:
//   1. view API          -> cid
//   2. finger/spi        -> buvid3 cookie (anti-bot)
//   3. web-interface/nav -> WBI image/sub keys
//   4. wbi/playurl       -> DASH audio stream URL(s)
//   5. download best audio -> local file (MediaSource::FILE)
class BilibiliExtractor : public IExtractor {
public:
    bool supports(const std::string& url) const override;
    const char* name() const override { return "bilibili"; }
    MediaSource extract(const std::string& url,
                        const ExtractOptions& opt,
                        const TempDir& tempDir) override;
};
