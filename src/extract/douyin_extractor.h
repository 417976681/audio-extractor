#pragma once

#include "extractor.h"

// Site-specific extractor for Douyin (抖音).
//
// Douyin serves its media through the aweme/iteminfo web API. Historically a
// plain request to that endpoint (with browser-like headers) returned a
// playable audio/video URL; newer deployments require a request signature
// (X-Bogus / _signature) that is computed in JavaScript, which this tool does
// NOT implement. This extractor therefore does a best-effort fetch and fails
// with a clear message if Douyin now rejects unsigned requests.
//
// It prefers the original music track (aweme.music.play_url) and falls back to
// the video's audio (aweme.video.play_addr). Both are downloaded and handed to
// the transcoder, which keeps only the audio track.
class DouyinExtractor : public IExtractor {
public:
    bool supports(const std::string& url) const override;
    const char* name() const override { return "douyin"; }
    MediaSource extract(const std::string& url,
                        const ExtractOptions& opt,
                        const TempDir& tempDir) override;
};
