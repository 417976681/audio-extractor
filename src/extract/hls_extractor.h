#pragma once

#include "extractor.h"

// Extractor for HLS (.m3u8) streams: parses the playlist(s), downloads each
// media segment via libcurl, and produces a concat list for the audio
// extractor. Handles master playlists (variant selection by bandwidth) and
// media playlists. Encrypted segments (AES-128 / SAMPLE-AES) are downloaded
// but NOT decrypted in this version.
class HlsExtractor : public IExtractor {
public:
    bool supports(const std::string& url) const override;
    const char* name() const override { return "hls"; }
    MediaSource extract(const std::string& url,
                        const ExtractOptions& opt,
                        const TempDir& tempDir) override;

private:
    // Recursively resolve a playlist; returns the final media-playlist text
    // and the base URL used to resolve its segments.
    std::string resolvePlaylist(const std::string& url,
                                const ExtractOptions& opt,
                                std::string& outBaseUrl);

    std::string resolveUrl(const std::string& base, const std::string& rel);
};
