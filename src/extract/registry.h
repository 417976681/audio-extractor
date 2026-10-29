#pragma once

#include "extractor.h"
#include "direct_extractor.h"
#include "hls_extractor.h"
#include "bilibili_extractor.h"
#include "youtube_extractor.h"
#include "douyin_extractor.h"
#include <vector>

// Simple static registry of extractors. Call initDefaults() once at startup.
inline std::vector<IExtractor*>& registryInstances() {
    static std::vector<IExtractor*> insts;
    return insts;
}

inline void ExtractorRegistry::registerExtractor(IExtractor* ex) {
    registryInstances().push_back(ex);
}

inline IExtractor* ExtractorRegistry::select(const std::string& url) {
    for (auto* ex : registryInstances())
        if (ex && ex->supports(url)) return ex;
    return nullptr;
}

inline void ExtractorRegistry::initDefaults() {
    static bool done = false;
    if (done) return;
    done = true;
    static DirectExtractor g_direct;
    static HlsExtractor g_hls;
    static BilibiliExtractor g_bili;
    static YouTubeExtractor g_youtube;
    static DouyinExtractor g_douyin;
    // HLS first (it only claims .m3u8 URLs), then the site-specific extractors
    // (Bilibili, YouTube, Douyin), then the universal direct fallback which
    // claims everything else. Order matters: the direct fallback must be LAST,
    // otherwise it would swallow a YouTube/Douyin page URL and try to download
    // the HTML as media.
    registerExtractor(&g_hls);
    registerExtractor(&g_bili);
    registerExtractor(&g_youtube);
    registerExtractor(&g_douyin);
    registerExtractor(&g_direct);
}
