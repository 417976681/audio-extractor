#pragma once

#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <cstdint>
#include <filesystem>

// Describes one audio file in the library.
struct TrackMeta {
    std::string id;          // stable hash of the absolute path
    std::string path;        // absolute path (UTF-8)
    std::string fileName;    // "song.mp3"
    std::string title;       // metadata title, falls back to the file stem
    std::string artist;
    std::string album;
    std::string genre;
    std::string codec;       // "mp3", "flac", ...
    int    year       = 0;
    double duration   = 0;   // seconds
    int64_t size      = 0;
    int    bitrateKbps = 0;
    int    sampleRate = 0;
    int    channels   = 0;
    int64_t mtime     = 0;   // last write time (seconds since epoch)
    bool   hasCover   = false;
};

// Scans one or more folders for audio files and reads their metadata and
// embedded cover art with FFmpeg. Results are cached; call scan(true) to force
// a re-read of the filesystem.
class MusicLibrary {
public:
    // Directories to scan.
    void setDirectories(const std::vector<std::string>& dirs);
    const std::vector<std::string>& directories() const { return dirs_; }
    bool addDirectory(const std::string& dir);
    bool removeDirectory(const std::string& dir);

    // Scan all directories (cached unless `force`). Tracks are returned with
    // the most recently modified files first.
    std::vector<TrackMeta> scan(bool force = false);

    // Look up a track by id (refreshes the cache only if the id is unknown).
    bool find(const std::string& id, TrackMeta& out);

    // Embedded cover art. Returns false when the track has none.
    bool cover(const std::string& id, std::vector<uint8_t>& out, std::string& mime);

    // Read metadata + cover for a single file (used internally, exposed for tests).
    static bool readFileMeta(const std::string& path, TrackMeta& out,
                             std::vector<uint8_t>* coverOut = nullptr,
                             std::string* coverMime = nullptr);

    // Sanitize a title so it can be used as a filename on Windows/POSIX.
    static std::string sanitizeFileName(const std::string& name);

    static std::string makeId(const std::string& absPath);

private:
    mutable std::mutex mu_;
    std::vector<std::string> dirs_;
    std::vector<TrackMeta> cache_;
    std::map<std::string, std::pair<std::string, std::vector<uint8_t>>> coverCache_; // id -> (mime, bytes)
    bool scanned_ = false;
};

// JSON helpers shared by the server (small and dependency free).
std::string jsonEscape(const std::string& s);
std::string trackToJson(const TrackMeta& t);
