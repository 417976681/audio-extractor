#pragma once

#include <string>
#include <curl/curl.h>
#include "common/common.h"

// Thin libcurl wrapper used by all extractors for HTTP(S) fetching.
// Supports custom User-Agent / Referer / Cookie and download progress.
class HttpClient {
public:
    // Must be called once before any request.
    static bool init();
    static void cleanup();

    // Download `url` to local file `outPath`. Returns 0 on success,
    // otherwise a CURLcode (or -1 for a filesystem error).
    // The HTTP status code is written to *httpStatus when non-null.
    static long downloadToFile(const std::string& url,
                               const std::string& outPath,
                               const ExtractOptions& opt,
                               long* httpStatus = nullptr);

    // Download `url` into `out` as a string (used for HLS manifests).
    static long downloadToString(const std::string& url,
                                 std::string& out,
                                 const ExtractOptions& opt,
                                 long* httpStatus = nullptr);

    // Follow redirects for `url` and return the final (effective) URL.
    // Used to expand short links such as b23.tv/<code> into the real page.
    // Returns a CURLcode (CURLE_OK on success); the resolved URL is written
    // to `finalUrl` (empty if it could not be determined).
    static long resolveRedirect(const std::string& url,
                               const ExtractOptions& opt,
                               std::string& finalUrl);

    // POST `body` (raw, e.g. JSON) to `url` and capture the response body into
    // `response`. `extraHeaders` is an optional list of {"Name", "Value"} pairs
    // (pass e.g. {"Content-Type", "application/json"}). Returns a CURLcode
    // (CURLE_OK on success). The HTTP status code is written to *httpStatus
    // when non-null. Used by the YouTube/TVHTML5 innertube flow.
    static long postToString(const std::string& url,
                             const std::string& body,
                             const ExtractOptions& opt,
                             std::string& response,
                             long* httpStatus = nullptr,
                             const std::vector<std::pair<std::string, std::string>>* extraHeaders = nullptr);
};
