#include "platform.h"

#ifdef _WIN32
// winsock2 must be pulled in before windows.h, otherwise the SDK falls back to
// the old winsock.h declarations. Same for iphlpapi: it needs winsock2 types.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <shellapi.h>
#include <cwchar>
#else
#include <ifaddrs.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <net/if.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <cstring>

namespace plat {

#ifdef _WIN32
std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (n == 0) return {};
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

std::string wideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                                nullptr, 0, nullptr, nullptr);
    if (n == 0) return {};
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                        s.data(), n, nullptr, nullptr);
    return s;
}
#endif

std::filesystem::path u8path(const std::string& utf8) {
#ifdef _WIN32
    return std::filesystem::path(utf8ToWide(utf8));
#else
    return std::filesystem::path(utf8);
#endif
}

std::string pathToUtf8(const std::filesystem::path& p) {
#ifdef _WIN32
    return wideToUtf8(p.wstring());
#else
    return p.string();
#endif
}

std::string withSuffix(const std::string& utf8Path, const std::string& suffixUtf8) {
#ifdef _WIN32
    std::filesystem::path p = u8path(utf8Path);
    std::filesystem::path np = p.parent_path() / (p.stem().wstring() + utf8ToWide(suffixUtf8));
    return pathToUtf8(np);
#else
    std::filesystem::path p = u8path(utf8Path);
    std::filesystem::path np = p.parent_path() / (p.stem().string() + suffixUtf8);
    return pathToUtf8(np);
#endif
}

FILE* openFileWrite(const std::string& utf8Path) {
#ifdef _WIN32
    return _wfopen(utf8ToWide(utf8Path).c_str(), L"wb");
#else
    return fopen(utf8Path.c_str(), "wb");
#endif
}

FILE* openFileRead(const std::string& utf8Path) {
#ifdef _WIN32
    return _wfopen(utf8ToWide(utf8Path).c_str(), L"rb");
#else
    return fopen(utf8Path.c_str(), "rb");
#endif
}

int renamePath(const std::string& fromUtf8, const std::string& toUtf8) {
#ifdef _WIN32
    return MoveFileExW(utf8ToWide(fromUtf8).c_str(),
                       utf8ToWide(toUtf8).c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED) ? 0 : -1;
#else
    return std::rename(fromUtf8.c_str(), toUtf8.c_str());
#endif
}

int seek64(FILE* f, int64_t offset, int whence) {
#ifdef _WIN32
    return _fseeki64(f, offset, whence);
#else
    return fseeko(f, offset, whence);
#endif
}

int64_t tell64(FILE* f) {
#ifdef _WIN32
    return _ftelli64(f);
#else
    return ftello(f);
#endif
}

std::vector<std::string> getCommandLineArgs(int argc, char** argv) {
#ifdef _WIN32
    int n = 0;
    wchar_t** wargv = CommandLineToArgvW(GetCommandLineW(), &n);
    std::vector<std::string> out;
    if (wargv) {
        out.reserve((size_t)n);
        for (int i = 0; i < n; ++i) out.push_back(wideToUtf8(wargv[i]));
        LocalFree(wargv);
    }
    return out;
#else
    std::vector<std::string> out;
    out.reserve((size_t)argc);
    for (int i = 0; i < argc; ++i) out.push_back(argv[i]);
    return out;
#endif
}

std::vector<std::string> lanIPv4s() {
    std::vector<std::string> out;
    auto keep = [&out](const std::string& ip) {
        if (ip.empty() || ip.rfind("127.", 0) == 0 || ip.rfind("169.254.", 0) == 0) return;
        if (std::find(out.begin(), out.end(), ip) != out.end()) return;
        out.push_back(ip);
    };

#ifdef _WIN32
    ULONG size = 16384;
    std::vector<char> buf(size);
    DWORD ret = ERROR_BUFFER_OVERFLOW;
    for (int attempt = 0; attempt < 4 && ret == ERROR_BUFFER_OVERFLOW; ++attempt) {
        ret = GetAdaptersAddresses(
            AF_INET,
            GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
            nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &size);
        if (ret == ERROR_BUFFER_OVERFLOW) buf.resize(size);
    }
    if (ret != NO_ERROR) return out;
    for (const IP_ADAPTER_ADDRESSES* a =
             reinterpret_cast<const IP_ADAPTER_ADDRESSES*>(buf.data());
         a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp) continue;
        if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (const IP_ADAPTER_UNICAST_ADDRESS* u = a->FirstUnicastAddress; u; u = u->Next) {
            if (!u->Address.lpSockaddr) continue;
            const sockaddr_in* sa = reinterpret_cast<const sockaddr_in*>(u->Address.lpSockaddr);
            if (sa->sin_family != AF_INET) continue;
            std::array<char, INET_ADDRSTRLEN> ip{};
            if (InetNtopA(AF_INET, const_cast<void*>(static_cast<const void*>(&sa->sin_addr)),
                          ip.data(), ip.size()))
                keep(std::string(ip.data()));
        }
    }
#else
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0) return out;
    for (ifaddrs* i = list; i; i = i->ifa_next) {
        if (!i->ifa_addr || (i->ifa_flags & IFF_UP) == 0) continue;
        if ((i->ifa_flags & IFF_LOOPBACK) != 0) continue;
        if (i->ifa_addr->sa_family != AF_INET) continue;
        char host[NI_MAXHOST] = {0};
        if (getnameinfo(i->ifa_addr, sizeof(sockaddr_in), host, sizeof(host),
                        nullptr, 0, NI_NUMERICHOST) == 0)
            keep(std::string(host));
    }
    freeifaddrs(list);
#endif
    return out;
}

std::string hostName() {
#ifdef _WIN32
    DWORD n = 0;
    GetComputerNameW(nullptr, &n);            // first call yields the length
    std::wstring w(n + 1, L'\0');
    if (GetComputerNameW(w.data(), &n)) return wideToUtf8(std::wstring(w.c_str()));
    return {};
#else
    std::array<char, 256> buf{};
    if (::gethostname(buf.data(), buf.size()) == 0) return std::string(buf.data());
    return {};
#endif
}

}  // namespace plat
