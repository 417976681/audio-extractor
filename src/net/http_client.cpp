#include "http_client.h"

#include <curl/curl.h>
#include <cstdio>
#include <stdexcept>
#include "common/platform.h"

#ifdef _WIN32
#include <windows.h>
inline unsigned int getCurrentPid() { return GetCurrentProcessId(); }
#else
#include <unistd.h>
inline unsigned int getCurrentPid() { return (unsigned int)getpid(); }
#endif

static size_t writeFileCb(void* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* f = static_cast<FILE*>(userdata);
    return fwrite(ptr, size, nmemb, f);
}

static size_t writeStringCb(void* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* s = static_cast<std::string*>(userdata);
    s->append(static_cast<const char*>(ptr), size * nmemb);
    return size * nmemb;
}

static int xferInfoCb(void* clientp, curl_off_t dltotal, curl_off_t dlnow,
                      curl_off_t, curl_off_t) {
    auto* opt = static_cast<const ExtractOptions*>(clientp);
    if (opt && opt->progress) {
        double frac = (dltotal > 0) ? (double)dlnow / (double)dltotal : -1.0;
        opt->progress(frac);
    }
    return 0;
}

bool HttpClient::init() {
    return curl_global_init(CURL_GLOBAL_ALL) == CURLE_OK;
}

void HttpClient::cleanup() {
    curl_global_cleanup();
}

static long perform(CURL* curl, const std::string& url, const ExtractOptions& opt,
                    long* httpStatus) {
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT,
                     opt.connectTimeoutSec > 0 ? opt.connectTimeoutSec : 30L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,
                     opt.timeoutSec > 0 ? opt.timeoutSec : 600L);
    // Disable automatic decompression so we get raw bytes.
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_USERAGENT, opt.userAgent.c_str());
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, xferInfoCb);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, (void*)&opt);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);

    // Never route loopback addresses through a (possibly present) HTTP proxy.
    // This keeps localhost mock servers / LAN addresses reachable and avoids
    // funneling them through an unrelated proxy.
    curl_easy_setopt(curl, CURLOPT_NOPROXY, "127.0.0.1,localhost,[::1]");

    struct curl_slist* headers = nullptr;
    if (!opt.referer.empty()) {
        std::string r = "Referer: " + opt.referer;
        headers = curl_slist_append(headers, r.c_str());
    }
    if (!opt.cookie.empty()) {
        std::string c = "Cookie: " + opt.cookie;
        headers = curl_slist_append(headers, c.c_str());
    }
    if (headers) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    CURLcode res = curl_easy_perform(curl);

    if (httpStatus) {
        long code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
        *httpStatus = code;
    }
    if (headers) curl_slist_free_all(headers);
    return (long)res;
}

long HttpClient::downloadToFile(const std::string& url, const std::string& outPath,
                                const ExtractOptions& opt, long* httpStatus) {
    CURL* curl = curl_easy_init();
    if (!curl) return -1;

    FILE* f = plat::openFileWrite(outPath);
    if (!f) {
        curl_easy_cleanup(curl);
        return -1;
    }

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeFileCb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void*)f);

    long res = perform(curl, url, opt, httpStatus);
    fclose(f);
    curl_easy_cleanup(curl);
    return res;
}

long HttpClient::downloadToString(const std::string& url, std::string& out,
                                  const ExtractOptions& opt, long* httpStatus) {
    CURL* curl = curl_easy_init();
    if (!curl) return -1;

    out.clear();
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeStringCb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void*)&out);

    long res = perform(curl, url, opt, httpStatus);
    curl_easy_cleanup(curl);
    return res;
}

long HttpClient::resolveRedirect(const std::string& url, const ExtractOptions& opt,
                                std::string& finalUrl) {
    CURL* curl = curl_easy_init();
    if (!curl) return -1;

    // We don't need the body; reuse the string writer into a throwaway buffer.
    std::string discard;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_USERAGENT, opt.userAgent.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeStringCb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void*)&discard);

    struct curl_slist* headers = nullptr;
    if (!opt.referer.empty()) {
        std::string r = "Referer: " + opt.referer;
        headers = curl_slist_append(headers, r.c_str());
    }
    if (!opt.cookie.empty()) {
        std::string c = "Cookie: " + opt.cookie;
        headers = curl_slist_append(headers, c.c_str());
    }
    if (headers) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    curl_easy_setopt(curl, CURLOPT_NOPROXY, "127.0.0.1,localhost,[::1]");

    CURLcode res = curl_easy_perform(curl);
    char* eff = nullptr;
    if (res == CURLE_OK)
        curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &eff);
    if (eff) finalUrl = eff;
    if (headers) curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return (long)res;
}

long HttpClient::postToString(const std::string& url, const std::string& body,
                              const ExtractOptions& opt, std::string& response,
                              long* httpStatus,
                              const std::vector<std::pair<std::string, std::string>>* extraHeaders) {
    CURL* curl = curl_easy_init();
    if (!curl) return -1;

    response.clear();
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body.size());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT,
                     opt.connectTimeoutSec > 0 ? opt.connectTimeoutSec : 30L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,
                     opt.timeoutSec > 0 ? opt.timeoutSec : 60L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_USERAGENT, opt.userAgent.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeStringCb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void*)&response);

    struct curl_slist* headers = nullptr;
    if (!opt.referer.empty()) {
        std::string r = "Referer: " + opt.referer;
        headers = curl_slist_append(headers, r.c_str());
    }
    if (!opt.cookie.empty()) {
        std::string c = "Cookie: " + opt.cookie;
        headers = curl_slist_append(headers, c.c_str());
    }
    if (extraHeaders) {
        for (const auto& h : *extraHeaders) {
            std::string line = h.first + ": " + h.second;
            headers = curl_slist_append(headers, line.c_str());
        }
    }
    if (headers) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    curl_easy_setopt(curl, CURLOPT_NOPROXY, "127.0.0.1,localhost,[::1]");

    CURLcode res = curl_easy_perform(curl);
    if (httpStatus) {
        long code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
        *httpStatus = code;
    }
    if (headers) curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return (long)res;
}
