#pragma once

#include <string>
#include <vector>
#include <cstdio>
#include <cstdint>
#include <filesystem>

// Cross-platform helpers that make the tool fully Unicode-aware on Windows.
//
// Internal string encoding: everything in the program is a UTF-8 std::string.
// On Windows the Win32/CRT APIs expect UTF-16, so this module converts at the
// boundary. On POSIX the native encoding is already UTF-8 and the helpers are
// pass-throughs.
//
// Without this, Chinese (and other non-ASCII) paths break on Windows because:
//   * argv comes in as the local ANSI codepage, not UTF-8.
//   * std::ifstream/ofstream and fopen() interpret narrow strings as ANSI.
//   * std::filesystem::path's narrow constructor also uses the ANSI codepage.
//   * FFmpeg's default file protocol may not accept a UTF-8 filename.

namespace plat {

#ifdef _WIN32
// Convert between UTF-8 (internal) and UTF-16 (Windows API).
std::wstring utf8ToWide(const std::string& s);
std::string  wideToUtf8(const std::wstring& w);
#endif

// Build a std::filesystem::path from a UTF-8 string (wide, on Windows).
std::filesystem::path u8path(const std::string& utf8);

// Convert a path back to a UTF-8 string (inverse of u8path).
std::string pathToUtf8(const std::filesystem::path& p);

// Return "<parent>/<stem><suffixUtf8>" as a UTF-8 string, preserving any
// non-ASCII characters in the original stem on every platform. `suffixUtf8`
// is appended verbatim (e.g. ".mp3" or "_2").
std::string withSuffix(const std::string& utf8Path, const std::string& suffixUtf8);

// Open a file for binary read/write using a UTF-8 path. Returns nullptr on
// failure. The caller must fclose() the returned handle.
FILE* openFileWrite(const std::string& utf8Path);
FILE* openFileRead(const std::string& utf8Path);

// Rename/move a file (UTF-8 paths). 0 on success, -1 on failure.
int renamePath(const std::string& fromUtf8, const std::string& toUtf8);

// 64-bit seek/tell on a FILE* (works around MinGW's 32-bit long).
int    seek64(FILE* f, int64_t offset, int whence);
int64_t tell64(FILE* f);

// Command-line arguments as UTF-8 strings. On Windows this uses
// GetCommandLineW + CommandLineToArgvW so Chinese arguments are preserved;
// on POSIX it just wraps argv.
std::vector<std::string> getCommandLineArgs(int argc, char** argv);

// IPv4 addresses this machine has on its local networks. Loopback (127.*) and
// link-local (169.254.*) addresses are excluded; the first entry is the one
// most likely to be reachable from other devices. Empty when offline.
std::vector<std::string> lanIPv4s();

// Human readable host name of the machine, or "" when unavailable.
std::string hostName();

}  // namespace plat
