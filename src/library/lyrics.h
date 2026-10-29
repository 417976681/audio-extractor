#pragma once

#include <string>
#include <vector>
#include <cstdint>

// ---------------------------------------------------------------------------
// Lyrics engine.
//
// Lyrics can come from three places, checked in this order:
//   1. a sibling .lrc file next to the audio file (always wins when present)
//   2. lyric tags embedded in the container/ID3 frames
//   3. one of the online providers (LRCLIB, NetEase Cloud Music)
//
// Once an online hit is chosen it is written next to the audio file as
// "<track stem>.lrc" so subsequent plays never hit the network again.
// ---------------------------------------------------------------------------

namespace lyr {

// One timestamped line. `time` is in seconds and already includes any
// LRC [offset:] value.
struct Line {
    double time = 0.0;
    std::string text;
};

struct Lyric {
    bool found = false;      // something usable was loaded
    bool synced = false;     // has timestamps (karaoke highlighting possible)
    std::string source;      // "local" | "embedded" | "lrclib" | "netease"
    std::string localPath;   // .lrc file backing this lyric ("" if none yet)
    double offset = 0.0;     // seconds, from [offset:]
    std::vector<Line> lines;

    std::string title;
    std::string artist;
    std::string album;

    std::string plain() const;                    // flattened text
    bool hasAnyText() const;
};

// ---------------------------------------------------------------------------
// LRC parsing / serialisation
// ---------------------------------------------------------------------------

// Parses standard LRC (including multi-timestamp lines such as
// "[00:12.00][01:20.00]text" and metadata tags [ti:]/[ar:]/[al:]/[offset:]).
Lyric parseLrc(const std::string& text);

// Renders to canonical LRC. Round-trips through parseLrc().
std::string toLrc(const Lyric& l);

// First `n` meaningful lines joined by " / " — used for search previews.
std::string previewOf(const Lyric& l, std::size_t n = 2);

// ---------------------------------------------------------------------------
// Local .lrc handling
// ---------------------------------------------------------------------------

// "<dir>/<stem>.lrc" for a given audio file path.
std::string lrcPathFor(const std::string& audioPath);

// Loads the sibling .lrc file if it exists. Returns false when absent.
bool loadLocal(const std::string& audioPath, Lyric& out);

// Writes `lrc` next to the audio file. Returns false on I/O failure.
bool saveLocal(const std::string& audioPath, const std::string& lrc,
               std::string* writtenPath = nullptr);

// Deletes the sibling .lrc file. Returns false if there was nothing to remove.
bool removeLocal(const std::string& audioPath);

// Reads lyrics stored inside the media file itself (USLT / lyrics-*` tags).
bool readEmbedded(const std::string& audioPath, Lyric& out);

// ---------------------------------------------------------------------------
// Online lookup
// ---------------------------------------------------------------------------

struct Query {
    std::string title;
    std::string artist;
    std::string album;
    double duration = 0.0;   // seconds, may be 0 when unknown
};

struct SearchHit {
    std::string provider;    // "lrclib" | "netease"
    std::string remoteId;
    std::string title;
    std::string artist;
    std::string album;
    double duration = 0.0;
    bool synced = false;     // timed lyrics available
    std::string preview;     // first couple of lines
    double score = 0.0;      // match quality, higher is better
};

// Queries every provider and merges the result set, best match first.
// Never throws: on network failure it returns whatever succeeded and
// reports details through `err`.
std::vector<SearchHit> search(const Query& q, int maxHits = 12,
                              std::string* err = nullptr);

// Downloads the full lyric text for a previously returned SearchHit.
// Returns false when the provider did not yield anything usable.
bool fetchByHit(const SearchHit& h, std::string& lrcOut, std::string* err = nullptr);

// Percent-encoded query-string value (space => %20, UTF-8 safe).
std::string urlEncode(const std::string& s);

} // namespace lyr
