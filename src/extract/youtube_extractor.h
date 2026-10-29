#pragma once

#include "extractor.h"

// Site-specific extractor for YouTube.
//
// YouTube does not expose a plain media URL on its watch pages; audio is
// delivered through the "innertube" (youtubei) player API as DASH/adaptive
// streams. This extractor:
//   1. extracts the 11-character video id from any YouTube URL shape;
//   2. calls /youtubei/v1/player with a sequence of clients, trying the
//      TVHTML5 ("tv") client FIRST — it is the client that returns direct
//      stream URLs without the signature / n-throttle dance, so we avoid
//      having to run YouTube's JS to decipher signatures;
//   3. picks the best audio-only adaptive format (highest bitrate);
//   4. downloads that stream to a local file (MediaSource::FILE).
//
// Note: the player endpoint can be redirected at build/runtime via the
// YT_API_BASE environment variable (used for local testing / debugging).
class YouTubeExtractor : public IExtractor {
public:
    bool supports(const std::string& url) const override;
    const char* name() const override { return "youtube"; }
    MediaSource extract(const std::string& url,
                        const ExtractOptions& opt,
                        const TempDir& tempDir) override;
};
