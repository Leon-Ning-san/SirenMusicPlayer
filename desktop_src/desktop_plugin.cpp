#include "desktop_plugin.h"
#include "util.h"
#include "plat.h"
#include "stb_image.h"

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "qrcodegen.h"
}

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <queue>
#include <sstream>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace sirene {
namespace {

constexpr int THUMB_PX = 160;

struct PluginEntry {
    PluginInfo info;
    std::map<std::string, std::string> kv;
    std::mutex call_mtx;
};

struct HttpResp {
    bool ok = false;
    int status = 0;
    std::string headers;
    std::string set_cookie;
    std::string body;
    std::string error;
};

std::string g_cfg_dir;
std::string g_plugins_dir;
std::string g_pdata_dir;
std::string g_stream_dir;
std::string g_cover_dir;

std::mutex g_mgr_mtx;
std::vector<PluginEntry*> g_plugins;
PluginUiSnapshot g_snap;
bool g_got_playlists = false;
bool g_got_tracks = false;
bool g_got_play = false;
PluginQrState g_qr;
std::atomic<bool> g_quit{ false };
std::atomic<int> g_active_jobs{ 0 };
std::atomic<uint64_t> g_req_gen{ 1 };

std::string safe_id_str(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('_');
        }
    }
    if (out.empty()) out = "plugin";
    return out;
}

uint64_t fnv1a_64(const std::string& s) {
    uint64_t h = 14695981039346656037ULL;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    return h;
}

std::string hash_hex_str(const std::string& s) {
    char buf[24]{};
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(fnv1a_64(s)));
    return std::string(buf);
}

#ifdef _WIN32
std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), &w[0], n);
    return w;
}

std::string wide_to_utf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), &s[0], n, nullptr, nullptr);
    return s;
}
#endif

bool read_file_bytes(const std::string& path, std::string& out) {
    FILE* f = plat_fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (sz < 0 || sz > 32 * 1024 * 1024) {
        std::fclose(f);
        return false;
    }
    out.resize(static_cast<size_t>(sz));
    if (sz > 0) {
        size_t n = std::fread(&out[0], 1, static_cast<size_t>(sz), f);
        out.resize(n);
    }
    std::fclose(f);
    return true;
}

bool write_file_bytes(const std::string& path, const char* data, size_t len) {
    FILE* f = plat_fopen(path.c_str(), "wb");
    if (!f) return false;
    bool ok = (len == 0) || (std::fwrite(data, 1, len, f) == len);
    std::fclose(f);
    return ok;
}

bool file_exists_nonempty(const std::string& path) {
    FILE* f = plat_fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fclose(f);
    return sz > 0;
}

void ensure_dir_recursive(const char* dir) {
    if (!dir || !*dir) return;
    std::string cur = dir;
    for (size_t i = 0; i < cur.size(); ++i) {
        if (cur[i] == '/' || cur[i] == '\\') {
            if (i == 0) continue;
            if (i == 2 && cur[1] == ':') continue;
            std::string sub = cur.substr(0, i);
#ifdef _WIN32
            CreateDirectoryW(utf8_to_wide(sub).c_str(), nullptr);
#else
            mkdir(sub.c_str(), 0755);
#endif
        }
    }
#ifdef _WIN32
    CreateDirectoryW(utf8_to_wide(cur).c_str(), nullptr);
#else
    mkdir(cur.c_str(), 0755);
#endif
}

// ---------------- Crypto & Encoding Helpers ----------------

std::string url_encode_str(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 2);
    for (unsigned char c : s) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[(c >> 4) & 0xF]);
            out.push_back(hex[c & 0xF]);
        }
    }
    return out;
}

std::string base64_encode_bytes(const unsigned char* data, size_t len) {
    static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = static_cast<uint32_t>(data[i]) << 16;
        if (i + 1 < len) v |= static_cast<uint32_t>(data[i + 1]) << 8;
        if (i + 2 < len) v |= static_cast<uint32_t>(data[i + 2]);
        out.push_back(tbl[(v >> 18) & 0x3F]);
        out.push_back(tbl[(v >> 12) & 0x3F]);
        out.push_back((i + 1 < len) ? tbl[(v >> 6) & 0x3F] : '=');
        out.push_back((i + 2 < len) ? tbl[v & 0x3F] : '=');
    }
    return out;
}

std::string base64_decode_str(const std::string& in) {
    int T[256];
    std::fill(std::begin(T), std::end(T), -1);
    for (int i = 0; i < 64; ++i) {
        T[static_cast<unsigned char>("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"[i])] = i;
    }
    std::string out;
    int val = 0, valb = -8;
    for (unsigned char c : in) {
        if (c == '=') break;
        int d = T[c];
        if (d == -1) continue;
        val = (val << 6) + d;
        valb += 6;
        if (valb >= 0) {
            out.push_back(static_cast<char>((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return out;
}

std::string md5_hex(const std::string& msg) {
    static const uint32_t K[64] = {
        0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
        0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
        0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
        0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
        0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
        0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
        0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
        0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391
    };
    static const int R[64] = {
        7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,
        5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,
        4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,
        6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21
    };
    std::vector<uint8_t> buf(msg.begin(), msg.end());
    uint64_t bitlen = static_cast<uint64_t>(buf.size()) * 8ULL;
    buf.push_back(0x80);
    while ((buf.size() % 64) != 56) buf.push_back(0);
    for (int i = 0; i < 8; ++i) buf.push_back(static_cast<uint8_t>((bitlen >> (8 * i)) & 0xFFu));

    uint32_t h0 = 0x67452301u, h1 = 0xefcdab89u, h2 = 0x98badcfeu, h3 = 0x10325476u;
    auto rotl = [](uint32_t x, int n) { return (x << n) | (x >> (32 - n)); };
    for (size_t off = 0; off < buf.size(); off += 64) {
        uint32_t w[16];
        for (int i = 0; i < 16; ++i) {
            size_t j = off + static_cast<size_t>(i) * 4;
            w[i] = static_cast<uint32_t>(buf[j]) | (static_cast<uint32_t>(buf[j + 1]) << 8) |
                   (static_cast<uint32_t>(buf[j + 2]) << 16) | (static_cast<uint32_t>(buf[j + 3]) << 24);
        }
        uint32_t a = h0, b = h1, c = h2, d = h3;
        for (int i = 0; i < 64; ++i) {
            uint32_t f = 0, g = 0;
            if (i < 16) { f = (b & c) | ((~b) & d); g = static_cast<uint32_t>(i); }
            else if (i < 32) { f = (d & b) | ((~d) & c); g = static_cast<uint32_t>((5 * i + 1) % 16); }
            else if (i < 48) { f = b ^ c ^ d; g = static_cast<uint32_t>((3 * i + 5) % 16); }
            else { f = c ^ (b | (~d)); g = static_cast<uint32_t>((7 * i) % 16); }
            uint32_t tmp = d;
            d = c; c = b;
            b = b + rotl(a + f + K[i] + w[g], R[i]);
            a = tmp;
        }
        h0 += a; h1 += b; h2 += c; h3 += d;
    }
    uint8_t dig[16]{};
    uint32_t hs[4] = { h0, h1, h2, h3 };
    for (int i = 0; i < 4; ++i) {
        dig[i * 4 + 0] = static_cast<uint8_t>(hs[i] & 0xFFu);
        dig[i * 4 + 1] = static_cast<uint8_t>((hs[i] >> 8) & 0xFFu);
        dig[i * 4 + 2] = static_cast<uint8_t>((hs[i] >> 16) & 0xFFu);
        dig[i * 4 + 3] = static_cast<uint8_t>((hs[i] >> 24) & 0xFFu);
    }
    static const char* hx = "0123456789abcdef";
    std::string out(32, '0');
    for (int i = 0; i < 16; ++i) {
        out[static_cast<size_t>(i) * 2]     = hx[(dig[i] >> 4) & 0xF];
        out[static_cast<size_t>(i) * 2 + 1] = hx[dig[i] & 0xF];
    }
    return out;
}

std::string sha256_hex(const std::string& msg) {
    static const uint32_t K[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
    };
    std::vector<uint8_t> buf(msg.begin(), msg.end());
    uint64_t bitlen = static_cast<uint64_t>(buf.size()) * 8ULL;
    buf.push_back(0x80);
    while ((buf.size() % 64) != 56) buf.push_back(0);
    for (int i = 7; i >= 0; --i) buf.push_back(static_cast<uint8_t>((bitlen >> (8 * i)) & 0xFFu));

    uint32_t h[8] = {
        0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19
    };
    auto rotr = [](uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };
    for (size_t off = 0; off < buf.size(); off += 64) {
        uint32_t w[64]{};
        for (int i = 0; i < 16; ++i) {
            size_t j = off + static_cast<size_t>(i) * 4;
            w[i] = (static_cast<uint32_t>(buf[j]) << 24) | (static_cast<uint32_t>(buf[j + 1]) << 16) |
                   (static_cast<uint32_t>(buf[j + 2]) << 8) | static_cast<uint32_t>(buf[j + 3]);
        }
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a=h[0], b=h[1], c=h[2], d=h[3], e=h[4], f=h[5], g=h[6], hh=h[7];
        for (int i = 0; i < 64; ++i) {
            uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            uint32_t ch = (e & f) ^ ((~e) & g);
            uint32_t temp1 = hh + S1 + ch + K[i] + w[i];
            uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t temp2 = S0 + maj;
            hh = g; g = f; f = e; e = d + temp1;
            d = c; c = b; b = a; a = temp1 + temp2;
        }
        h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
    }
    static const char* hx = "0123456789abcdef";
    std::string out(64, '0');
    for (int i = 0; i < 8; ++i) {
        for (int j = 0; j < 4; ++j) {
            uint8_t byte = static_cast<uint8_t>((h[i] >> ((3 - j) * 8)) & 0xFFu);
            out[static_cast<size_t>(i * 8 + j * 2)]     = hx[(byte >> 4) & 0xF];
            out[static_cast<size_t>(i * 8 + j * 2 + 1)] = hx[byte & 0xF];
        }
    }
    return out;
}

// ---------------- Cross-Platform HTTP Engine ----------------

HttpResp http_request_sync(const std::string& method, const std::string& url,
                           const std::vector<std::pair<std::string, std::string>>& headers,
                           const std::string& body) {
    HttpResp res;
    if (url.empty()) { res.error = "empty url"; return res; }
#ifdef _WIN32
    typedef HINTERNET (WINAPI *PFN_WinHttpOpen)(LPCWSTR, DWORD, LPCWSTR, LPCWSTR, DWORD);
    typedef HINTERNET (WINAPI *PFN_WinHttpConnect)(HINTERNET, LPCWSTR, WORD, DWORD);
    typedef HINTERNET (WINAPI *PFN_WinHttpOpenRequest)(HINTERNET, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR*, DWORD);
    typedef BOOL (WINAPI *PFN_WinHttpAddRequestHeaders)(HINTERNET, LPCWSTR, DWORD, DWORD);
    typedef BOOL (WINAPI *PFN_WinHttpSendRequest)(HINTERNET, LPCWSTR, DWORD, LPVOID, DWORD, DWORD, DWORD_PTR);
    typedef BOOL (WINAPI *PFN_WinHttpReceiveResponse)(HINTERNET, LPVOID);
    typedef BOOL (WINAPI *PFN_WinHttpQueryHeaders)(HINTERNET, DWORD, LPCWSTR, LPVOID, LPDWORD, LPDWORD);
    typedef BOOL (WINAPI *PFN_WinHttpQueryDataAvailable)(HINTERNET, LPDWORD);
    typedef BOOL (WINAPI *PFN_WinHttpReadData)(HINTERNET, LPVOID, DWORD, LPDWORD);
    typedef BOOL (WINAPI *PFN_WinHttpCloseHandle)(HINTERNET);
    typedef BOOL (WINAPI *PFN_WinHttpSetTimeouts)(HINTERNET, int, int, int, int);
    typedef BOOL (WINAPI *PFN_WinHttpCrackUrl)(LPCWSTR, DWORD, DWORD, LPURL_COMPONENTS);

    HMODULE hMod = GetModuleHandleW(L"winhttp.dll");
    if (!hMod) hMod = LoadLibraryW(L"winhttp.dll");
    if (!hMod) { res.error = "winhttp.dll unavailable"; return res; }

    auto pOpen    = reinterpret_cast<PFN_WinHttpOpen>(reinterpret_cast<void*>(GetProcAddress(hMod, "WinHttpOpen")));
    auto pConn    = reinterpret_cast<PFN_WinHttpConnect>(reinterpret_cast<void*>(GetProcAddress(hMod, "WinHttpConnect")));
    auto pReq     = reinterpret_cast<PFN_WinHttpOpenRequest>(reinterpret_cast<void*>(GetProcAddress(hMod, "WinHttpOpenRequest")));
    auto pAddHdr  = reinterpret_cast<PFN_WinHttpAddRequestHeaders>(reinterpret_cast<void*>(GetProcAddress(hMod, "WinHttpAddRequestHeaders")));
    auto pSend    = reinterpret_cast<PFN_WinHttpSendRequest>(reinterpret_cast<void*>(GetProcAddress(hMod, "WinHttpSendRequest")));
    auto pRecv    = reinterpret_cast<PFN_WinHttpReceiveResponse>(reinterpret_cast<void*>(GetProcAddress(hMod, "WinHttpReceiveResponse")));
    auto pQryHdr  = reinterpret_cast<PFN_WinHttpQueryHeaders>(reinterpret_cast<void*>(GetProcAddress(hMod, "WinHttpQueryHeaders")));
    auto pAvail   = reinterpret_cast<PFN_WinHttpQueryDataAvailable>(reinterpret_cast<void*>(GetProcAddress(hMod, "WinHttpQueryDataAvailable")));
    auto pRead    = reinterpret_cast<PFN_WinHttpReadData>(reinterpret_cast<void*>(GetProcAddress(hMod, "WinHttpReadData")));
    auto pClose   = reinterpret_cast<PFN_WinHttpCloseHandle>(reinterpret_cast<void*>(GetProcAddress(hMod, "WinHttpCloseHandle")));
    auto pTimeout = reinterpret_cast<PFN_WinHttpSetTimeouts>(reinterpret_cast<void*>(GetProcAddress(hMod, "WinHttpSetTimeouts")));
    auto pCrack   = reinterpret_cast<PFN_WinHttpCrackUrl>(reinterpret_cast<void*>(GetProcAddress(hMod, "WinHttpCrackUrl")));
    if (!pOpen || !pConn || !pReq || !pSend || !pRecv || !pQryHdr || !pAvail || !pRead || !pClose || !pCrack) {
        res.error = "winhttp symbols missing";
        return res;
    }

    std::wstring wurl = utf8_to_wide(url);
    wchar_t whost[256]{};
    wchar_t wpath[4096]{};
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    uc.lpszHostName = whost; uc.dwHostNameLength = 256;
    uc.lpszUrlPath = wpath;  uc.dwUrlPathLength = 4096;
    if (!pCrack(wurl.c_str(), static_cast<DWORD>(wurl.size()), 0, &uc) || !whost[0]) {
        res.error = "invalid url";
        return res;
    }

    std::wstring wmeth = utf8_to_wide(method.empty() ? "GET" : method);
    HINTERNET hSes = pOpen(L"SirenePluginHost/1.2", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSes) { res.error = "WinHttpOpen failed"; return res; }
    if (pTimeout) pTimeout(hSes, 8000, 8000, 12000, 20000);

    HINTERNET hCon = pConn(hSes, whost, uc.nPort, 0);
    if (!hCon) { pClose(hSes); res.error = "WinHttpConnect failed"; return res; }

    const DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hReq = pReq(hCon, wmeth.c_str(), wpath[0] ? wpath : L"/", nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hReq) { pClose(hCon); pClose(hSes); res.error = "WinHttpOpenRequest failed"; return res; }

    if (pAddHdr && !headers.empty()) {
        std::string hblock;
        for (const auto& kv : headers) {
            hblock += kv.first + ": " + kv.second + "\r\n";
        }
        std::wstring wh = utf8_to_wide(hblock);
        pAddHdr(hReq, wh.c_str(), static_cast<DWORD>(wh.size()), WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
    }

    LPVOID bptr = body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data());
    DWORD blen = static_cast<DWORD>(body.size());
    if (!pSend(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0, bptr, blen, blen, 0) || !pRecv(hReq, nullptr)) {
        pClose(hReq); pClose(hCon); pClose(hSes);
        res.error = "http send/recv failed";
        return res;
    }

    DWORD status = 0, st_sz = sizeof(status);
    pQryHdr(hReq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &st_sz, WINHTTP_NO_HEADER_INDEX);
    res.status = static_cast<int>(status);

    DWORD hdr_sz = 0;
    pQryHdr(hReq, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &hdr_sz, WINHTTP_NO_HEADER_INDEX);
    if (hdr_sz > 0 && hdr_sz < 65536) {
        std::wstring whdr(hdr_sz / sizeof(wchar_t) + 1, L'\0');
        if (pQryHdr(hReq, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, &whdr[0], &hdr_sz, WINHTTP_NO_HEADER_INDEX)) {
            res.headers = wide_to_utf8(whdr.c_str());
            std::istringstream iss(res.headers);
            std::string line;
            while (std::getline(iss, line)) {
                if (line.size() > 11 &&
                    (line.compare(0, 11, "Set-Cookie:") == 0 || line.compare(0, 11, "set-cookie:") == 0)) {
                    std::string ck = line.substr(11);
                    while (!ck.empty() && (ck.front() == ' ' || ck.front() == '\t')) ck.erase(ck.begin());
                    auto semi = ck.find(';');
                    if (semi != std::string::npos) ck = ck.substr(0, semi);
                    if (!ck.empty()) {
                        if (!res.set_cookie.empty()) res.set_cookie += "; ";
                        res.set_cookie += ck;
                    }
                }
            }
        }
    }

    std::string out_body;
    DWORD avail = 0;
    while (pAvail(hReq, &avail) && avail > 0) {
        if (out_body.size() + avail > 64u * 1024u * 1024u) break;
        std::vector<char> chunk(avail);
        DWORD got = 0;
        if (!pRead(hReq, chunk.data(), avail, &got) || got == 0) break;
        out_body.append(chunk.data(), got);
    }
    pClose(hReq); pClose(hCon); pClose(hSes);
    res.body = std::move(out_body);
    res.ok = (res.status >= 200 && res.status < 400);
    return res;
#else
    std::string tmp_hdr = "/tmp/sirene_hdr_" + hash_hex_str(url) + ".txt";
    std::string tmp_body = "/tmp/sirene_req_" + hash_hex_str(url) + ".bin";
    std::string tmp_out = "/tmp/sirene_res_" + hash_hex_str(url) + ".bin";
    bool has_body = !body.empty();
    if (has_body) write_file_bytes(tmp_body, body.data(), body.size());

    std::vector<std::string> args = {
        "curl", "-sSL", "--max-time", "20",
        "-D", tmp_hdr, "-o", tmp_out,
        "-X", method.empty() ? "GET" : method
    };
    for (const auto& kv : headers) {
        args.push_back("-H");
        args.push_back(kv.first + ": " + kv.second);
    }
    if (has_body) {
        args.push_back("--data-binary");
        args.push_back("@" + tmp_body);
    }
    args.push_back(url);

    pid_t pid = fork();
    if (pid == 0) {
        std::vector<char*> cargv;
        for (auto& s : args) cargv.push_back(const_cast<char*>(s.c_str()));
        cargv.push_back(nullptr);
        execvp("curl", cargv.data());
        _exit(127);
    } else if (pid > 0) {
        int st = 0;
        waitpid(pid, &st, 0);
        if (WIFEXITED(st) && WEXITSTATUS(st) == 0) {
            read_file_bytes(tmp_out, res.body);
            read_file_bytes(tmp_hdr, res.headers);
            std::istringstream iss(res.headers);
            std::string line;
            while (std::getline(iss, line)) {
                if (line.compare(0, 5, "HTTP/") == 0) {
                    auto sp = line.find(' ');
                    if (sp != std::string::npos) res.status = std::atoi(line.c_str() + sp + 1);
                } else if (line.size() > 11 &&
                           (line.compare(0, 11, "Set-Cookie:") == 0 || line.compare(0, 11, "set-cookie:") == 0)) {
                    std::string ck = line.substr(11);
                    while (!ck.empty() && (ck.front() == ' ' || ck.front() == '\t')) ck.erase(ck.begin());
                    auto semi = ck.find(';');
                    if (semi != std::string::npos) ck = ck.substr(0, semi);
                    if (!ck.empty()) {
                        if (!res.set_cookie.empty()) res.set_cookie += "; ";
                        res.set_cookie += ck;
                    }
                }
            }
            res.ok = (res.status >= 200 && res.status < 400);
        } else {
            res.error = "curl failed";
        }
    } else {
        res.error = "fork failed";
    }
    std::remove(tmp_hdr.c_str());
    std::remove(tmp_body.c_str());
    std::remove(tmp_out.c_str());
    return res;
#endif
}

// ---------------- JSON <-> Lua Bridge ----------------

struct JsonParser {
    const char* p = nullptr;
    const char* end = nullptr;

    void skip_ws() {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p;
    }

    static void append_utf8(std::string& out, uint32_t cp) {
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    bool parse_hex4(uint32_t& out) {
        if (p + 4 > end) return false;
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            char c = p[i];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<uint32_t>(c - 'A' + 10);
            else return false;
        }
        p += 4;
        out = v;
        return true;
    }

    bool parse_string(std::string& out) {
        if (p >= end || *p != '"') return false;
        ++p;
        out.clear();
        while (p < end) {
            char c = *p++;
            if (c == '"') return true;
            if (c == '\\') {
                if (p >= end) return false;
                char e = *p++;
                switch (e) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    uint32_t cp = 0;
                    if (!parse_hex4(cp)) return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF && p + 2 <= end && p[0] == '\\' && p[1] == 'u') {
                        p += 2;
                        uint32_t low = 0;
                        if (parse_hex4(low) && low >= 0xDC00 && low <= 0xDFFF) {
                            cp = 0x10000u + (((cp - 0xD800u) << 10) | (low - 0xDC00u));
                        }
                    }
                    append_utf8(out, cp);
                    break;
                }
                default: out.push_back(e); break;
                }
            } else {
                out.push_back(c);
            }
        }
        return false;
    }

    bool parse_value(lua_State* L, int depth = 0) {
        if (depth > 48) return false;
        skip_ws();
        if (p >= end) return false;
        char c = *p;
        if (c == '{') {
            ++p;
            lua_newtable(L);
            skip_ws();
            if (p < end && *p == '}') { ++p; return true; }
            while (p < end) {
                skip_ws();
                std::string k;
                if (!parse_string(k)) { lua_pop(L, 1); return false; }
                skip_ws();
                if (p >= end || *p != ':') { lua_pop(L, 1); return false; }
                ++p;
                lua_pushlstring(L, k.data(), k.size());
                if (!parse_value(L, depth + 1)) { lua_pop(L, 2); return false; }
                lua_rawset(L, -3);
                skip_ws();
                if (p < end && *p == ',') { ++p; continue; }
                if (p < end && *p == '}') { ++p; return true; }
                lua_pop(L, 1);
                return false;
            }
            lua_pop(L, 1);
            return false;
        }
        if (c == '[') {
            ++p;
            lua_newtable(L);
            skip_ws();
            if (p < end && *p == ']') { ++p; return true; }
            lua_Integer idx = 1;
            while (p < end) {
                if (!parse_value(L, depth + 1)) { lua_pop(L, 1); return false; }
                lua_rawseti(L, -2, idx++);
                skip_ws();
                if (p < end && *p == ',') { ++p; continue; }
                if (p < end && *p == ']') { ++p; return true; }
                lua_pop(L, 1);
                return false;
            }
            lua_pop(L, 1);
            return false;
        }
        if (c == '"') {
            std::string s;
            if (!parse_string(s)) return false;
            lua_pushlstring(L, s.data(), s.size());
            return true;
        }
        if (c == 't' && end - p >= 4 && std::memcmp(p, "true", 4) == 0) {
            p += 4; lua_pushboolean(L, 1); return true;
        }
        if (c == 'f' && end - p >= 5 && std::memcmp(p, "false", 5) == 0) {
            p += 5; lua_pushboolean(L, 0); return true;
        }
        if (c == 'n' && end - p >= 4 && std::memcmp(p, "null", 4) == 0) {
            p += 4; lua_pushnil(L); return true;
        }
        if (c == '-' || (c >= '0' && c <= '9')) {
            const char* start = p;
            bool is_float = false;
            if (*p == '-') ++p;
            while (p < end && *p >= '0' && *p <= '9') ++p;
            if (p < end && *p == '.') { is_float = true; ++p; while (p < end && *p >= '0' && *p <= '9') ++p; }
            if (p < end && (*p == 'e' || *p == 'E')) {
                is_float = true; ++p;
                if (p < end && (*p == '+' || *p == '-')) ++p;
                while (p < end && *p >= '0' && *p <= '9') ++p;
            }
            std::string num(start, static_cast<size_t>(p - start));
            if (is_float) lua_pushnumber(L, std::strtod(num.c_str(), nullptr));
            else          lua_pushinteger(L, std::strtoll(num.c_str(), nullptr, 10));
            return true;
        }
        return false;
    }
};

void json_escape_str(std::string& out, const char* s, size_t len) {
    out.push_back('"');
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8]{};
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out.push_back(static_cast<char>(c));
            }
        }
    }
    out.push_back('"');
}

void lua_to_json(lua_State* L, int idx, std::string& out, int depth = 0) {
    if (depth > 32) { out += "null"; return; }
    int t = lua_type(L, idx);
    if (t == LUA_TNIL || t == LUA_TNONE) { out += "null"; return; }
    if (t == LUA_TBOOLEAN) { out += (lua_toboolean(L, idx) ? "true" : "false"); return; }
    if (t == LUA_TNUMBER) {
        if (lua_isinteger(L, idx)) {
            out += std::to_string(lua_tointeger(L, idx));
        } else {
            char buf[48]{};
            std::snprintf(buf, sizeof(buf), "%.10g", lua_tonumber(L, idx));
            out += buf;
        }
        return;
    }
    if (t == LUA_TSTRING) {
        size_t len = 0;
        const char* s = lua_tolstring(L, idx, &len);
        json_escape_str(out, s, len);
        return;
    }
    if (t == LUA_TTABLE) {
        int abs_idx = lua_absindex(L, idx);
        lua_Integer arr_n = static_cast<lua_Integer>(lua_rawlen(L, abs_idx));
        bool is_arr = (arr_n > 0);
        if (is_arr) {
            lua_pushnil(L);
            while (lua_next(L, abs_idx) != 0) {
                if (lua_type(L, -2) != LUA_TNUMBER || !lua_isinteger(L, -2)) {
                    is_arr = false; lua_pop(L, 2); break;
                }
                lua_Integer k = lua_tointeger(L, -2);
                if (k < 1 || k > arr_n) { is_arr = false; lua_pop(L, 2); break; }
                lua_pop(L, 1);
            }
        }
        if (is_arr) {
            out.push_back('[');
            for (lua_Integer i = 1; i <= arr_n; ++i) {
                if (i > 1) out.push_back(',');
                lua_rawgeti(L, abs_idx, i);
                lua_to_json(L, -1, out, depth + 1);
                lua_pop(L, 1);
            }
            out.push_back(']');
        } else {
            out.push_back('{');
            bool first = true;
            lua_pushnil(L);
            while (lua_next(L, abs_idx) != 0) {
                if (lua_type(L, -2) == LUA_TSTRING) {
                    if (!first) out.push_back(',');
                    first = false;
                    size_t kl = 0;
                    const char* ks = lua_tolstring(L, -2, &kl);
                    json_escape_str(out, ks, kl);
                    out.push_back(':');
                    lua_to_json(L, -1, out, depth + 1);
                }
                lua_pop(L, 1);
            }
            out.push_back('}');
        }
        return;
    }
    out += "null";
}

// ---------------- Lua Host Functions ----------------

int l_sirene_http(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    std::string method = "GET", url, body;
    std::vector<std::pair<std::string, std::string>> hdrs;

    lua_getfield(L, 1, "method");
    if (lua_isstring(L, -1)) method = lua_tostring(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, 1, "url");
    if (lua_isstring(L, -1)) url = lua_tostring(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, 1, "body");
    if (lua_isstring(L, -1)) {
        size_t bl = 0;
        const char* bs = lua_tolstring(L, -1, &bl);
        body.assign(bs, bl);
    }
    lua_pop(L, 1);

    lua_getfield(L, 1, "headers");
    if (lua_istable(L, -1)) {
        lua_pushnil(L);
        while (lua_next(L, -2) != 0) {
            if (lua_isstring(L, -2) && lua_isstring(L, -1)) {
                hdrs.emplace_back(lua_tostring(L, -2), lua_tostring(L, -1));
            }
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);

    HttpResp resp = http_request_sync(method, url, hdrs, body);
    lua_newtable(L);
    lua_pushboolean(L, resp.ok ? 1 : 0);
    lua_setfield(L, -2, "ok");
    lua_pushinteger(L, resp.status);
    lua_setfield(L, -2, "status");
    lua_pushlstring(L, resp.body.data(), resp.body.size());
    lua_setfield(L, -2, "body");
    lua_pushlstring(L, resp.set_cookie.data(), resp.set_cookie.size());
    lua_setfield(L, -2, "set_cookie");
    lua_pushlstring(L, resp.headers.data(), resp.headers.size());
    lua_setfield(L, -2, "headers");
    lua_pushstring(L, resp.error.c_str());
    lua_setfield(L, -2, "error");
    return 1;
}

int l_sirene_json_decode(lua_State* L) {
    size_t len = 0;
    const char* s = luaL_checklstring(L, 1, &len);
    JsonParser jp{ s, s + len };
    if (!jp.parse_value(L)) {
        lua_pushnil(L);
        lua_pushstring(L, "invalid json");
        return 2;
    }
    return 1;
}

int l_sirene_json_encode(lua_State* L) {
    luaL_checkany(L, 1);
    std::string out;
    lua_to_json(L, 1, out);
    lua_pushlstring(L, out.data(), out.size());
    return 1;
}

int l_sirene_kv_get(lua_State* L) {
    auto* entry = static_cast<PluginEntry*>(lua_touserdata(L, lua_upvalueindex(1)));
    const char* k = luaL_checkstring(L, 1);
    const char* def = lua_isstring(L, 2) ? lua_tostring(L, 2) : "";
    auto it = entry->kv.find(k);
    if (it != entry->kv.end()) lua_pushlstring(L, it->second.data(), it->second.size());
    else                       lua_pushstring(L, def);
    return 1;
}

void save_entry_kv(const PluginEntry* entry) {
    if (g_pdata_dir.empty() || !entry || entry->info.id.empty()) return;
    ensure_dir_recursive(g_pdata_dir.c_str());
    std::string path = g_pdata_dir + "/" + safe_id_str(entry->info.id) + ".kv";
    FILE* f = plat_fopen(path.c_str(), "wb");
    if (!f) return;
    for (const auto& kv : entry->kv) {
        std::string v = kv.second;
        for (char& ch : v) if (ch == '\n' || ch == '\r') ch = ' ';
        std::fprintf(f, "%s=%s\n", kv.first.c_str(), v.c_str());
    }
    std::fclose(f);
}

void load_entry_kv(PluginEntry* entry) {
    if (g_pdata_dir.empty() || !entry || entry->info.id.empty()) return;
    std::string path = g_pdata_dir + "/" + safe_id_str(entry->info.id) + ".kv";
    FILE* f = plat_fopen(path.c_str(), "rb");
    if (!f) return;
    char line[4096]{};
    while (std::fgets(line, sizeof(line), f)) {
        size_t len = std::strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = 0;
        char* eq = std::strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        entry->kv[line] = eq + 1;
    }
    std::fclose(f);
}

int l_sirene_kv_set(lua_State* L) {
    auto* entry = static_cast<PluginEntry*>(lua_touserdata(L, lua_upvalueindex(1)));
    const char* k = luaL_checkstring(L, 1);
    if (lua_isnil(L, 2)) {
        entry->kv.erase(k);
    } else {
        size_t vl = 0;
        const char* vs = luaL_checklstring(L, 2, &vl);
        entry->kv[k] = std::string(vs, vl);
    }
    save_entry_kv(entry);
    return 0;
}

int l_sirene_md5(lua_State* L) {
    size_t len = 0;
    const char* s = luaL_checklstring(L, 1, &len);
    std::string h = md5_hex(std::string(s, len));
    lua_pushlstring(L, h.data(), h.size());
    return 1;
}

int l_sirene_sha256(lua_State* L) {
    size_t len = 0;
    const char* s = luaL_checklstring(L, 1, &len);
    std::string h = sha256_hex(std::string(s, len));
    lua_pushlstring(L, h.data(), h.size());
    return 1;
}

int l_sirene_base64_enc(lua_State* L) {
    size_t len = 0;
    const char* s = luaL_checklstring(L, 1, &len);
    std::string b = base64_encode_bytes(reinterpret_cast<const unsigned char*>(s), len);
    lua_pushlstring(L, b.data(), b.size());
    return 1;
}

int l_sirene_base64_dec(lua_State* L) {
    size_t len = 0;
    const char* s = luaL_checklstring(L, 1, &len);
    std::string b = base64_decode_str(std::string(s, len));
    lua_pushlstring(L, b.data(), b.size());
    return 1;
}

int l_sirene_url_enc(lua_State* L) {
    size_t len = 0;
    const char* s = luaL_checklstring(L, 1, &len);
    std::string e = url_encode_str(std::string(s, len));
    lua_pushlstring(L, e.data(), e.size());
    return 1;
}

int l_sirene_log(lua_State* L) {
    auto* entry = static_cast<PluginEntry*>(lua_touserdata(L, lua_upvalueindex(1)));
    const char* msg = luaL_optstring(L, 1, "");
    if (entry && msg) entry->info.last_error = msg;
    return 0;
}

void instruction_limit_hook(lua_State* L, lua_Debug*) {
    luaL_error(L, "plugin execution step limit exceeded");
}

lua_State* create_sandboxed_vm(PluginEntry* entry, const std::string& script_path, std::string& out_err) {
    lua_State* L = luaL_newstate();
    if (!L) { out_err = "luaL_newstate failed"; return nullptr; }
    // Safe sandboxed libraries (no os/io/package.loadlib)
    luaL_requiref(L, "_G", luaopen_base, 1);        lua_pop(L, 1);
    luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1);   lua_pop(L, 1);
    luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1);  lua_pop(L, 1);
    luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1);   lua_pop(L, 1);
    luaL_requiref(L, LUA_UTF8LIBNAME, luaopen_utf8, 1);   lua_pop(L, 1);
    luaL_requiref(L, LUA_COLIBNAME, luaopen_coroutine, 1); lua_pop(L, 1);

    lua_newtable(L);
    lua_pushcfunction(L, l_sirene_http);       lua_setfield(L, -2, "http");
    lua_pushcfunction(L, l_sirene_json_decode); lua_setfield(L, -2, "json_decode");
    lua_pushcfunction(L, l_sirene_json_encode); lua_setfield(L, -2, "json_encode");
    lua_pushcfunction(L, l_sirene_md5);        lua_setfield(L, -2, "md5");
    lua_pushcfunction(L, l_sirene_sha256);     lua_setfield(L, -2, "sha256");
    lua_pushcfunction(L, l_sirene_base64_enc); lua_setfield(L, -2, "base64_encode");
    lua_pushcfunction(L, l_sirene_base64_dec); lua_setfield(L, -2, "base64_decode");
    lua_pushcfunction(L, l_sirene_url_enc);    lua_setfield(L, -2, "url_encode");

    lua_pushlightuserdata(L, entry); lua_pushcclosure(L, l_sirene_kv_get, 1); lua_setfield(L, -2, "kv_get");
    lua_pushlightuserdata(L, entry); lua_pushcclosure(L, l_sirene_kv_set, 1); lua_setfield(L, -2, "kv_set");
    lua_pushlightuserdata(L, entry); lua_pushcclosure(L, l_sirene_log, 1);    lua_setfield(L, -2, "log");
    lua_setglobal(L, "sirene");

    std::string code;
    if (!read_file_bytes(script_path, code)) {
        out_err = "cannot read script: " + script_path;
        lua_close(L);
        return nullptr;
    }

    lua_sethook(L, instruction_limit_hook, LUA_MASKCOUNT, 5000000);
    if (luaL_loadbuffer(L, code.data(), code.size(), script_path.c_str()) != LUA_OK ||
        lua_pcall(L, 0, 1, 0) != LUA_OK) {
        out_err = lua_isstring(L, -1) ? lua_tostring(L, -1) : "lua load error";
        lua_close(L);
        return nullptr;
    }
    if (lua_istable(L, -1)) {
        lua_setglobal(L, "plugin");
    } else {
        lua_pop(L, 1);
    }
    lua_getglobal(L, "plugin");
    if (!lua_istable(L, -1)) {
        out_err = "plugin must define global or return 'plugin' table";
        lua_close(L);
        return nullptr;
    }
    lua_pop(L, 1);
    return L;
}

std::string lua_str_field(lua_State* L, int tbl_idx, const char* k, const char* def = "") {
    lua_getfield(L, tbl_idx, k);
    std::string v = lua_isstring(L, -1) ? lua_tostring(L, -1) : def;
    lua_pop(L, 1);
    return v;
}

double lua_num_field(lua_State* L, int tbl_idx, const char* k, double def = 0.0) {
    lua_getfield(L, tbl_idx, k);
    double v = lua_isnumber(L, -1) ? lua_tonumber(L, -1) : def;
    lua_pop(L, 1);
    return v;
}

bool lua_bool_field(lua_State* L, int tbl_idx, const char* k, bool def = false) {
    lua_getfield(L, tbl_idx, k);
    bool v = lua_isboolean(L, -1) ? (lua_toboolean(L, -1) != 0) : def;
    lua_pop(L, 1);
    return v;
}

void parse_track_from_lua(lua_State* L, int idx, const std::string& pid, OnlineTrack& t) {
    t.plugin_id = pid;
    t.id = lua_str_field(L, idx, "id");
    t.title = lua_str_field(L, idx, "title", "未知歌曲");
    t.artist = lua_str_field(L, idx, "artist");
    t.album = lua_str_field(L, idx, "album");
    t.cover_url = lua_str_field(L, idx, "cover_url");
    t.dur = lua_num_field(L, idx, "duration", 0.0);
    t.liked = lua_bool_field(L, idx, "liked", false);
    std::string fmt = lua_str_field(L, idx, "fmt", "STREAM");
    for (char& ch : fmt) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    std::snprintf(t.fmt, sizeof(t.fmt), "%s", fmt.c_str());
}

void save_plugins_state_locked() {
    if (g_cfg_dir.empty()) return;
    std::string path = g_cfg_dir + "/plugins_state.cfg";
    FILE* f = plat_fopen(path.c_str(), "wb");
    if (!f) return;
    for (const auto* p : g_plugins) {
        std::fprintf(f, "%s=%d\n", p->info.id.c_str(), p->info.enabled ? 1 : 0);
    }
    std::fclose(f);
}

std::map<std::string, bool> load_plugins_state() {
    std::map<std::string, bool> m;
    if (g_cfg_dir.empty()) return m;
    std::string path = g_cfg_dir + "/plugins_state.cfg";
    FILE* f = plat_fopen(path.c_str(), "rb");
    if (!f) return m;
    char line[512]{};
    while (std::fgets(line, sizeof(line), f)) {
        char* eq = std::strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        m[line] = (std::atoi(eq + 1) != 0);
    }
    std::fclose(f);
    return m;
}

void inspect_plugin_entry(PluginEntry* entry) {
    std::string err;
    lua_State* L = create_sandboxed_vm(entry, entry->info.path, err);
    if (!L) {
        entry->info.valid = false;
        entry->info.last_error = err;
        return;
    }
    lua_getglobal(L, "plugin");
    int t = lua_gettop(L);
    std::string pid = lua_str_field(L, t, "id");
    if (pid.empty()) pid = safe_id_str(entry->info.path);
    entry->info.id = pid;
    entry->info.name = lua_str_field(L, t, "name", pid.c_str());
    entry->info.version = lua_str_field(L, t, "version", "1.0.0");
    entry->info.author = lua_str_field(L, t, "author");
    entry->info.description = lua_str_field(L, t, "description");
    entry->info.supports_login = lua_bool_field(L, t, "supports_login", false);

    entry->info.config_fields.clear();
    lua_getfield(L, t, "config_fields");
    if (lua_istable(L, -1)) {
        int cf_idx = lua_gettop(L);
        lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L, cf_idx));
        for (lua_Integer i = 1; i <= n; ++i) {
            lua_rawgeti(L, cf_idx, i);
            if (lua_istable(L, -1)) {
                int it = lua_gettop(L);
                PluginConfigField cf;
                cf.key = lua_str_field(L, it, "key");
                cf.label = lua_str_field(L, it, "label", cf.key.c_str());
                cf.placeholder = lua_str_field(L, it, "placeholder");
                if (!cf.key.empty()) entry->info.config_fields.push_back(std::move(cf));
            }
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
    lua_close(L);

    load_entry_kv(entry);
    for (auto& cf : entry->info.config_fields) {
        auto it = entry->kv.find(cf.key);
        if (it != entry->kv.end()) cf.value = it->second;
    }
    auto it_nick = entry->kv.find("__nickname");
    if (it_nick != entry->kv.end() && !it_nick->second.empty()) {
        entry->info.logged_in = true;
        entry->info.nickname = it_nick->second;
    }
    entry->info.valid = true;
}

std::vector<std::string> scan_lua_scripts_in_dir(const std::string& dir) {
    std::vector<std::string> out;
    if (dir.empty()) return out;
#ifdef _WIN32
    std::wstring pat = utf8_to_wide(dir + "/*");
    WIN32_FIND_DATAW fd{};
    HANDLE hf = FindFirstFileW(pat.c_str(), &fd);
    if (hf == INVALID_HANDLE_VALUE) return out;
    do {
        if (fd.cFileName[0] == L'.') continue;
        std::string name = wide_to_utf8(fd.cFileName);
        std::string full = dir + "/" + name;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            std::string main_lua = full + "/main.lua";
            if (file_exists_nonempty(main_lua)) out.push_back(main_lua);
        } else if (name.size() > 4 && name.compare(name.size() - 4, 4, ".lua") == 0) {
            out.push_back(full);
        }
    } while (FindNextFileW(hf, &fd));
    FindClose(hf);
#else
    DIR* d = opendir(dir.c_str());
    if (!d) return out;
    struct dirent* de = nullptr;
    while ((de = readdir(d)) != nullptr) {
        if (de->d_name[0] == '.') continue;
        std::string name = de->d_name;
        std::string full = dir + "/" + name;
        struct stat st{};
        if (stat(full.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            std::string main_lua = full + "/main.lua";
            if (file_exists_nonempty(main_lua)) out.push_back(main_lua);
        } else if (name.size() > 4 && name.compare(name.size() - 4, 4, ".lua") == 0) {
            out.push_back(full);
        }
    }
    closedir(d);
#endif
    std::sort(out.begin(), out.end());
    return out;
}

PluginEntry* find_entry_copy_locked(const std::string& pid, std::string* out_path = nullptr,
                                    std::map<std::string, std::string>* out_kv = nullptr) {
    for (auto* p : g_plugins) {
        if (p->info.id == pid) {
            if (out_path) *out_path = p->info.path;
            if (out_kv) *out_kv = p->kv;
            return p;
        }
    }
    return nullptr;
}

void push_qr_bitmap_from_url(const std::string& url, PluginQrState& st) {
    uint8_t tempBuf[qrcodegen_BUFFER_LEN_MAX]{};
    uint8_t qrBuf[qrcodegen_BUFFER_LEN_MAX]{};
    if (qrcodegen_encodeText(url.c_str(), tempBuf, qrBuf, qrcodegen_Ecc_MEDIUM,
                             qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX,
                             qrcodegen_Mask_AUTO, true)) {
        int sz = qrcodegen_getSize(qrBuf);
        st.qr_size = sz;
        st.modules.assign(static_cast<size_t>(sz) * sz, 0);
        for (int y = 0; y < sz; ++y) {
            for (int x = 0; x < sz; ++x) {
                st.modules[static_cast<size_t>(y) * sz + x] = qrcodegen_getModule(qrBuf, x, y) ? 1 : 0;
            }
        }
    }
}

void write_readme_hint_if_empty() {
    std::string hint = g_plugins_dir + "/README.txt";
    FILE* chk = plat_fopen(hint.c_str(), "rb");
    if (chk) { std::fclose(chk); return; }
    const char* txt =
        "Sirène 外部扩展插件目录\r\n"
        "=====================\r\n"
        "请将外部扩展脚本（*.lua）或包含 main.lua 的扩展文件夹放入此目录，\r\n"
        "也可以直接将 .lua 文件拖入 Sirène 窗口完成导入。\r\n"
        "脚本编写规范请参考项目文档 docs/PLUGIN_SDK.md。\r\n";
    write_file_bytes(hint, txt, std::strlen(txt));
}

} // namespace

void plugin_init(const char* config_dir) {
    g_cfg_dir = (config_dir && *config_dir) ? config_dir : ".";
    g_plugins_dir = g_cfg_dir + "/plugins";
    g_pdata_dir   = g_cfg_dir + "/plugin_data";
    g_stream_dir  = g_cfg_dir + "/stream_cache";
    g_cover_dir   = g_cfg_dir + "/cover_cache";
    ensure_dir_recursive(g_plugins_dir.c_str());
    ensure_dir_recursive(g_pdata_dir.c_str());
    ensure_dir_recursive(g_stream_dir.c_str());
    ensure_dir_recursive(g_cover_dir.c_str());
    write_readme_hint_if_empty();
    plugin_rescan();
}

void plugin_shutdown() {
    g_quit.store(true);
    for (int i = 0; i < 60 && g_active_jobs.load() > 0; ++i) {
        plat_sleep_ms(25);
    }
    std::lock_guard<std::mutex> lk(g_mgr_mtx);
    for (auto* p : g_plugins) delete p;
    g_plugins.clear();
}

void plugin_rescan() {
    std::vector<std::string> files = scan_lua_scripts_in_dir(g_plugins_dir);
    std::map<std::string, bool> st_map = load_plugins_state();

    std::vector<PluginEntry*> next;
    for (const auto& fpath : files) {
        auto* e = new PluginEntry();
        e->info.path = fpath;
        inspect_plugin_entry(e);
        auto it = st_map.find(e->info.id);
        e->info.enabled = (it != st_map.end()) ? it->second : true;
        next.push_back(e);
    }

    std::lock_guard<std::mutex> lk(g_mgr_mtx);
    for (auto* old : g_plugins) delete old;
    g_plugins = std::move(next);
    g_snap.plugins_updated = true;
}

bool plugin_import_file(const std::string& src_path, std::string* out_err) {
    std::string content;
    if (!read_file_bytes(src_path, content)) {
        if (out_err) *out_err = "无法读取插件文件";
        return false;
    }
    const char* slash = std::strrchr(src_path.c_str(), '/');
    const char* bslash = std::strrchr(src_path.c_str(), '\\');
    const char* bn = slash > bslash ? (slash ? slash + 1 : src_path.c_str()) : (bslash ? bslash + 1 : src_path.c_str());
    std::string dst = g_plugins_dir + "/" + bn;
    if (!write_file_bytes(dst, content.data(), content.size())) {
        if (out_err) *out_err = "写入插件目录失败";
        return false;
    }
    plugin_rescan();
    return true;
}

bool plugin_remove(const std::string& plugin_id) {
    std::string path;
    {
        std::lock_guard<std::mutex> lk(g_mgr_mtx);
        for (const auto* p : g_plugins) {
            if (p->info.id == plugin_id) { path = p->info.path; break; }
        }
    }
    if (path.empty()) return false;
    std::remove(path.c_str());
    plugin_rescan();
    return true;
}

void plugin_set_enabled(const std::string& plugin_id, bool enabled) {
    std::lock_guard<std::mutex> lk(g_mgr_mtx);
    for (auto* p : g_plugins) {
        if (p->info.id == plugin_id) { p->info.enabled = enabled; break; }
    }
    save_plugins_state_locked();
    g_snap.plugins_updated = true;
}

void plugin_set_config(const std::string& plugin_id, const std::string& key, const std::string& val) {
    std::lock_guard<std::mutex> lk(g_mgr_mtx);
    for (auto* p : g_plugins) {
        if (p->info.id == plugin_id) {
            p->kv[key] = val;
            save_entry_kv(p);
            for (auto& cf : p->info.config_fields) if (cf.key == key) cf.value = val;
            break;
        }
    }
    g_snap.plugins_updated = true;
}

std::string plugin_dir_path() { return g_plugins_dir; }

int plugin_clear_cache() {
    int removed = 0;
    for (const auto& dir : { g_stream_dir, g_cover_dir }) {
#ifdef _WIN32
        std::wstring pat = utf8_to_wide(dir + "/*");
        WIN32_FIND_DATAW fd{};
        HANDLE hf = FindFirstFileW(pat.c_str(), &fd);
        if (hf != INVALID_HANDLE_VALUE) {
            do {
                if (fd.cFileName[0] == L'.' || (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                std::wstring wf = utf8_to_wide(dir + "/") + fd.cFileName;
                if (DeleteFileW(wf.c_str())) ++removed;
            } while (FindNextFileW(hf, &fd));
            FindClose(hf);
        }
#else
        DIR* d = opendir(dir.c_str());
        if (d) {
            struct dirent* de = nullptr;
            while ((de = readdir(d)) != nullptr) {
                if (de->d_name[0] == '.') continue;
                std::string full = dir + "/" + de->d_name;
                if (std::remove(full.c_str()) == 0) ++removed;
            }
            closedir(d);
        }
#endif
    }
    return removed;
}

std::vector<PluginInfo> plugin_list() {
    std::lock_guard<std::mutex> lk(g_mgr_mtx);
    std::vector<PluginInfo> out;
    out.reserve(g_plugins.size());
    for (const auto* p : g_plugins) out.push_back(p->info);
    return out;
}

int plugin_active_count() {
    std::lock_guard<std::mutex> lk(g_mgr_mtx);
    int n = 0;
    for (const auto* p : g_plugins) if (p->info.enabled && p->info.valid) ++n;
    return n;
}

std::string plugin_default_id() {
    std::lock_guard<std::mutex> lk(g_mgr_mtx);
    for (const auto* p : g_plugins) if (p->info.enabled && p->info.valid) return p->info.id;
    return std::string();
}

void plugin_async_search(const std::string& plugin_id, const std::string& query, int page) {
    if (plugin_id.empty()) return;
    uint64_t gen = ++g_req_gen;
    std::string spath;
    std::map<std::string, std::string> kv;
    {
        std::lock_guard<std::mutex> lk(g_mgr_mtx);
        if (!find_entry_copy_locked(plugin_id, &spath, &kv)) return;
        g_snap.is_busy = true;
    }
    ++g_active_jobs;
    std::thread([plugin_id, spath, kv, query, page, gen]() {
        PluginEntry local_ent;
        local_ent.info.id = plugin_id;
        local_ent.info.path = spath;
        local_ent.kv = kv;
        std::string err;
        std::vector<OnlineTrack> res_tracks;
        lua_State* L = create_sandboxed_vm(&local_ent, spath, err);
        if (L) {
            lua_getglobal(L, "plugin");
            lua_getfield(L, -1, "search");
            if (lua_isfunction(L, -1)) {
                lua_pushstring(L, query.c_str());
                lua_pushinteger(L, page);
                if (lua_pcall(L, 2, 1, 0) == LUA_OK && lua_istable(L, -1)) {
                    int arr = lua_gettop(L);
                    lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L, arr));
                    for (lua_Integer i = 1; i <= n && i <= 200; ++i) {
                        lua_rawgeti(L, arr, i);
                        if (lua_istable(L, -1)) {
                            OnlineTrack t;
                            parse_track_from_lua(L, lua_gettop(L), plugin_id, t);
                            if (!t.id.empty()) res_tracks.push_back(std::move(t));
                        }
                        lua_pop(L, 1);
                    }
                } else {
                    err = lua_isstring(L, -1) ? lua_tostring(L, -1) : "search failed";
                }
            }
            lua_close(L);
        }
        if (!g_quit.load() && gen == g_req_gen.load()) {
            std::lock_guard<std::mutex> lk(g_mgr_mtx);
            g_snap.tracks = std::move(res_tracks);
            g_snap.active_playlist_name = query.empty() ? "在线探索" : ("搜索: " + query);
            g_snap.is_busy = false;
            if (!err.empty()) g_snap.status_toast = err;
            g_got_tracks = true;
        }
        --g_active_jobs;
    }).detach();
}

void plugin_async_home(const std::string& plugin_id) {
    if (plugin_id.empty()) return;
    uint64_t gen = ++g_req_gen;
    std::string spath;
    std::map<std::string, std::string> kv;
    {
        std::lock_guard<std::mutex> lk(g_mgr_mtx);
        if (!find_entry_copy_locked(plugin_id, &spath, &kv)) return;
        g_snap.is_busy = true;
    }
    ++g_active_jobs;
    std::thread([plugin_id, spath, kv, gen]() {
        PluginEntry local_ent;
        local_ent.info.id = plugin_id;
        local_ent.info.path = spath;
        local_ent.kv = kv;
        std::string err;
        std::vector<OnlinePlaylist> pls;
        std::vector<OnlineTrack> favs;
        lua_State* L = create_sandboxed_vm(&local_ent, spath, err);
        if (L) {
            lua_getglobal(L, "plugin");
            int pt = lua_gettop(L);
            lua_getfield(L, pt, "get_playlists");
            if (lua_isfunction(L, -1)) {
                if (lua_pcall(L, 0, 1, 0) == LUA_OK && lua_istable(L, -1)) {
                    int arr = lua_gettop(L);
                    lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L, arr));
                    for (lua_Integer i = 1; i <= n && i <= 80; ++i) {
                        lua_rawgeti(L, arr, i);
                        if (lua_istable(L, -1)) {
                            int it = lua_gettop(L);
                            OnlinePlaylist pl;
                            pl.plugin_id = plugin_id;
                            pl.id = lua_str_field(L, it, "id");
                            pl.name = lua_str_field(L, it, "name", "歌单");
                            pl.description = lua_str_field(L, it, "description");
                            pl.cover_url = lua_str_field(L, it, "cover_url");
                            pl.category = lua_str_field(L, it, "category", "recommend");
                            pl.track_count = static_cast<int>(lua_num_field(L, it, "track_count", 0.0));
                            if (!pl.id.empty()) pls.push_back(std::move(pl));
                        }
                        lua_pop(L, 1);
                    }
                } else {
                    lua_pop(L, 1);
                }
            } else {
                lua_pop(L, 1);
            }

            lua_getfield(L, pt, "get_favorites");
            if (lua_isfunction(L, -1)) {
                if (lua_pcall(L, 0, 1, 0) == LUA_OK && lua_istable(L, -1)) {
                    int arr = lua_gettop(L);
                    lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L, arr));
                    for (lua_Integer i = 1; i <= n && i <= 200; ++i) {
                        lua_rawgeti(L, arr, i);
                        if (lua_istable(L, -1)) {
                            OnlineTrack t;
                            parse_track_from_lua(L, lua_gettop(L), plugin_id, t);
                            if (!t.id.empty()) favs.push_back(std::move(t));
                        }
                        lua_pop(L, 1);
                    }
                } else {
                    lua_pop(L, 1);
                }
            } else {
                lua_pop(L, 1);
            }
            lua_close(L);
        }
        if (!g_quit.load() && gen == g_req_gen.load()) {
            std::lock_guard<std::mutex> lk(g_mgr_mtx);
            g_snap.playlists = std::move(pls);
            if (!favs.empty()) {
                g_snap.tracks = std::move(favs);
                g_snap.active_playlist_name = "我的喜欢 / 推荐";
                g_got_tracks = true;
            }
            g_snap.is_busy = false;
            if (!err.empty()) g_snap.status_toast = err;
            g_got_playlists = true;
        }
        --g_active_jobs;
    }).detach();
}

void plugin_async_playlist_tracks(const std::string& plugin_id, const std::string& playlist_id) {
    if (plugin_id.empty() || playlist_id.empty()) return;
    uint64_t gen = ++g_req_gen;
    std::string spath, pl_name = "歌单详情";
    std::map<std::string, std::string> kv;
    {
        std::lock_guard<std::mutex> lk(g_mgr_mtx);
        if (!find_entry_copy_locked(plugin_id, &spath, &kv)) return;
        for (const auto& p : g_snap.playlists) if (p.id == playlist_id) { pl_name = p.name; break; }
        g_snap.is_busy = true;
    }
    ++g_active_jobs;
    std::thread([plugin_id, spath, kv, playlist_id, pl_name, gen]() {
        PluginEntry local_ent;
        local_ent.info.id = plugin_id;
        local_ent.info.path = spath;
        local_ent.kv = kv;
        std::string err;
        std::vector<OnlineTrack> res_tracks;
        lua_State* L = create_sandboxed_vm(&local_ent, spath, err);
        if (L) {
            lua_getglobal(L, "plugin");
            lua_getfield(L, -1, "get_playlist_tracks");
            if (lua_isfunction(L, -1)) {
                lua_pushstring(L, playlist_id.c_str());
                if (lua_pcall(L, 1, 1, 0) == LUA_OK && lua_istable(L, -1)) {
                    int arr = lua_gettop(L);
                    lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L, arr));
                    for (lua_Integer i = 1; i <= n && i <= 300; ++i) {
                        lua_rawgeti(L, arr, i);
                        if (lua_istable(L, -1)) {
                            OnlineTrack t;
                            parse_track_from_lua(L, lua_gettop(L), plugin_id, t);
                            if (!t.id.empty()) res_tracks.push_back(std::move(t));
                        }
                        lua_pop(L, 1);
                    }
                }
            }
            lua_close(L);
        }
        if (!g_quit.load() && gen == g_req_gen.load()) {
            std::lock_guard<std::mutex> lk(g_mgr_mtx);
            g_snap.tracks = std::move(res_tracks);
            g_snap.active_playlist_name = pl_name;
            g_snap.is_busy = false;
            if (!err.empty()) g_snap.status_toast = err;
            g_got_tracks = true;
        }
        --g_active_jobs;
    }).detach();
}

void plugin_async_play_track(const std::string& plugin_id, const OnlineTrack& track, const std::string& quality) {
    if (plugin_id.empty() || track.id.empty()) return;
    std::string spath;
    std::map<std::string, std::string> kv;
    {
        std::lock_guard<std::mutex> lk(g_mgr_mtx);
        if (!find_entry_copy_locked(plugin_id, &spath, &kv)) return;
        g_snap.is_busy = true;
    }
    ++g_active_jobs;
    std::thread([plugin_id, spath, kv, track, quality]() {
        PluginReadyPlay rp;
        rp.ready = true;
        rp.plugin_id = plugin_id;
        rp.track = track;

        PluginEntry local_ent;
        local_ent.info.id = plugin_id;
        local_ent.info.path = spath;
        local_ent.kv = kv;

        std::string err, play_url, ext = "mp3";
        std::vector<std::pair<std::string, std::string>> hdrs;
        lua_State* L = create_sandboxed_vm(&local_ent, spath, err);
        if (L) {
            lua_getglobal(L, "plugin");
            int pt = lua_gettop(L);
            lua_getfield(L, pt, "get_track_url");
            if (lua_isfunction(L, -1)) {
                lua_pushstring(L, track.id.c_str());
                lua_pushstring(L, quality.c_str());
                if (lua_pcall(L, 2, 1, 0) == LUA_OK) {
                    if (lua_isstring(L, -1)) {
                        play_url = lua_tostring(L, -1);
                    } else if (lua_istable(L, -1)) {
                        int rt = lua_gettop(L);
                        play_url = lua_str_field(L, rt, "url");
                        std::string f = lua_str_field(L, rt, "fmt");
                        if (!f.empty()) {
                            std::snprintf(rp.track.fmt, sizeof(rp.track.fmt), "%s", f.c_str());
                            for (char& ch : f) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                            ext = f;
                        }
                        lua_getfield(L, rt, "headers");
                        if (lua_istable(L, -1)) {
                            lua_pushnil(L);
                            while (lua_next(L, -2) != 0) {
                                if (lua_isstring(L, -2) && lua_isstring(L, -1)) {
                                    hdrs.emplace_back(lua_tostring(L, -2), lua_tostring(L, -1));
                                }
                                lua_pop(L, 1);
                            }
                        }
                        lua_pop(L, 1);
                    }
                }
                lua_pop(L, 1);
            } else {
                lua_pop(L, 1);
            }

            lua_getfield(L, pt, "get_lyrics");
            if (lua_isfunction(L, -1)) {
                lua_pushstring(L, track.id.c_str());
                if (lua_pcall(L, 1, 1, 0) == LUA_OK) {
                    if (lua_isstring(L, -1)) {
                        rp.lrc_orig = lua_tostring(L, -1);
                    } else if (lua_istable(L, -1)) {
                        rp.lrc_orig = lua_str_field(L, -1, "lrc");
                        rp.lrc_trans = lua_str_field(L, -1, "tlyric");
                    }
                }
                lua_pop(L, 1);
            } else {
                lua_pop(L, 1);
            }
            lua_close(L);
        }

        if (play_url.empty()) {
            rp.ok = false;
            rp.error = err.empty() ? "扩展未返回有效的音频播放地址" : err;
        } else {
            ensure_dir_recursive(g_stream_dir.c_str());
            if (ext == "mp3" || ext == "stream") {
                if (play_url.find(".flac") != std::string::npos) ext = "flac";
                else if (play_url.find(".m4a") != std::string::npos) ext = "m4a";
                else if (play_url.find(".ogg") != std::string::npos) ext = "ogg";
                else if (play_url.find(".wav") != std::string::npos) ext = "wav";
            }
            std::string cache_path = g_stream_dir + "/" + safe_id_str(plugin_id) + "_" + hash_hex_str(track.id) + "." + ext;
            if (!file_exists_nonempty(cache_path)) {
                HttpResp dl = http_request_sync("GET", play_url, hdrs, "");
                if (dl.ok && !dl.body.empty()) {
                    write_file_bytes(cache_path, dl.body.data(), dl.body.size());
                    rp.ok = true;
                    rp.audio_path = cache_path;
                } else {
                    rp.ok = false;
                    rp.error = "在线音频下载失败 (HTTP " + std::to_string(dl.status) + ")";
                }
            } else {
                rp.ok = true;
                rp.audio_path = cache_path;
            }
        }

        if (rp.ok && !track.cover_url.empty()) {
            ensure_dir_recursive(g_cover_dir.c_str());
            std::string cov_path = g_cover_dir + "/" + hash_hex_str(track.cover_url) + ".img";
            if (!file_exists_nonempty(cov_path)) {
                HttpResp ci = http_request_sync("GET", track.cover_url, {}, "");
                if (ci.ok && !ci.body.empty()) write_file_bytes(cov_path, ci.body.data(), ci.body.size());
            }
            if (file_exists_nonempty(cov_path)) rp.cover_path = cov_path;
        }

        if (!g_quit.load()) {
            std::lock_guard<std::mutex> lk(g_mgr_mtx);
            g_snap.ready_play = std::move(rp);
            g_snap.is_busy = false;
            g_got_play = true;
        }
        --g_active_jobs;
    }).detach();
}

void plugin_async_thumb(const std::string& plugin_id, size_t track_list_idx, const std::string& url) {
    if (url.empty()) return;
    ++g_active_jobs;
    std::thread([plugin_id, track_list_idx, url]() {
        ensure_dir_recursive(g_cover_dir.c_str());
        std::string cov_path = g_cover_dir + "/" + hash_hex_str(url) + ".img";
        std::string bytes;
        if (!read_file_bytes(cov_path, bytes)) {
            HttpResp ci = http_request_sync("GET", url, {}, "");
            if (ci.ok && !ci.body.empty()) {
                write_file_bytes(cov_path, ci.body.data(), ci.body.size());
                bytes = std::move(ci.body);
            }
        }
        std::vector<uint32_t> rgba;
        if (!bytes.empty()) {
            int w = 0, h = 0, ch = 0;
            unsigned char* img = stbi_load_from_memory(reinterpret_cast<const unsigned char*>(bytes.data()),
                                                       static_cast<int>(bytes.size()), &w, &h, &ch, 4);
            if (img && w > 0 && h > 0) {
                rgba.resize(static_cast<size_t>(THUMB_PX) * THUMB_PX);
                int side = std::min(w, h);
                int sx0 = (w - side) / 2;
                int sy0 = (h - side) / 2;
                for (int y = 0; y < THUMB_PX; ++y) {
                    int sy = sy0 + (y * side) / THUMB_PX;
                    for (int x = 0; x < THUMB_PX; ++x) {
                        int sx = sx0 + (x * side) / THUMB_PX;
                        const unsigned char* sp = img + (static_cast<size_t>(sy) * w + sx) * 4;
                        rgba[static_cast<size_t>(y) * THUMB_PX + x] =
                            0xFF000000u | (static_cast<uint32_t>(sp[2]) << 16) |
                            (static_cast<uint32_t>(sp[1]) << 8) | static_cast<uint32_t>(sp[0]);
                    }
                }
            }
            if (img) stbi_image_free(img);
        }
        if (!g_quit.load()) {
            std::lock_guard<std::mutex> lk(g_mgr_mtx);
            if (track_list_idx < g_snap.tracks.size() &&
                g_snap.tracks[track_list_idx].plugin_id == plugin_id &&
                g_snap.tracks[track_list_idx].cover_url == url) {
                if (!rgba.empty()) {
                    g_snap.tracks[track_list_idx].thumb = std::move(rgba);
                    g_snap.tracks[track_list_idx].thumb_state = 2;
                } else {
                    g_snap.tracks[track_list_idx].thumb_state = 3;
                }
                g_got_tracks = true;
            }
        }
        --g_active_jobs;
    }).detach();
}

void plugin_qr_start(const std::string& plugin_id) {
    std::string spath;
    std::map<std::string, std::string> kv;
    {
        std::lock_guard<std::mutex> lk(g_mgr_mtx);
        if (!find_entry_copy_locked(plugin_id, &spath, &kv)) return;
        g_qr = PluginQrState{};
        g_qr.active = true;
        g_qr.plugin_id = plugin_id;
        g_qr.status = "loading";
        g_qr.message = "正在请求登录二维码...";
    }
    ++g_active_jobs;
    std::thread([plugin_id, spath, kv]() {
        PluginEntry local_ent;
        local_ent.info.id = plugin_id;
        local_ent.info.path = spath;
        local_ent.kv = kv;
        std::string err, qr_key, qr_url, msg;
        lua_State* L = create_sandboxed_vm(&local_ent, spath, err);
        if (L) {
            lua_getglobal(L, "plugin");
            lua_getfield(L, -1, "login_qr_start");
            if (lua_isfunction(L, -1)) {
                if (lua_pcall(L, 0, 1, 0) == LUA_OK && lua_istable(L, -1)) {
                    qr_key = lua_str_field(L, -1, "key");
                    qr_url = lua_str_field(L, -1, "url");
                    msg    = lua_str_field(L, -1, "message", "请打开对应手机客户端扫码登录");
                } else {
                    err = lua_isstring(L, -1) ? lua_tostring(L, -1) : "login_qr_start failed";
                }
            } else {
                err = "该扩展未实现扫码登录 (login_qr_start)";
            }
            lua_close(L);
        }
        if (!g_quit.load()) {
            std::lock_guard<std::mutex> lk(g_mgr_mtx);
            if (g_qr.active && g_qr.plugin_id == plugin_id) {
                if (!qr_url.empty()) {
                    g_qr.qr_key = qr_key;
                    g_qr.qr_url = qr_url;
                    g_qr.status = "waiting";
                    g_qr.message = msg;
                    push_qr_bitmap_from_url(qr_url, g_qr);
                } else {
                    g_qr.status = "error";
                    g_qr.message = err.empty() ? "无法获取二维码地址" : err;
                }
            }
        }
        --g_active_jobs;
    }).detach();
}

void plugin_qr_poll() {
    std::string pid, key, spath;
    std::map<std::string, std::string> kv;
    {
        std::lock_guard<std::mutex> lk(g_mgr_mtx);
        if (!g_qr.active || (g_qr.status != "waiting" && g_qr.status != "scanned") || g_qr.qr_key.empty()) return;
        pid = g_qr.plugin_id;
        key = g_qr.qr_key;
        if (!find_entry_copy_locked(pid, &spath, &kv)) return;
    }
    ++g_active_jobs;
    std::thread([pid, key, spath, kv]() {
        PluginEntry local_ent;
        local_ent.info.id = pid;
        local_ent.info.path = spath;
        local_ent.kv = kv;
        std::string err, st = "waiting", msg, nick;
        lua_State* L = create_sandboxed_vm(&local_ent, spath, err);
        if (L) {
            lua_getglobal(L, "plugin");
            lua_getfield(L, -1, "login_qr_poll");
            if (lua_isfunction(L, -1)) {
                lua_pushstring(L, key.c_str());
                if (lua_pcall(L, 1, 1, 0) == LUA_OK && lua_istable(L, -1)) {
                    st   = lua_str_field(L, -1, "status", "waiting");
                    msg  = lua_str_field(L, -1, "message");
                    nick = lua_str_field(L, -1, "nickname");
                }
            }
            lua_close(L);
        }
        if (!g_quit.load()) {
            std::lock_guard<std::mutex> lk(g_mgr_mtx);
            // Sync any KV entries saved by Lua login_qr_poll back into live plugin entry
            for (auto* p : g_plugins) {
                if (p->info.id == pid) {
                    p->kv = local_ent.kv;
                    if (st == "ok") {
                        p->info.logged_in = true;
                        p->info.nickname = nick.empty() ? "已登录账号" : nick;
                        p->kv["__nickname"] = p->info.nickname;
                        save_entry_kv(p);
                        g_snap.plugins_updated = true;
                    }
                    break;
                }
            }
            if (g_qr.active && g_qr.plugin_id == pid) {
                g_qr.status = st;
                if (!msg.empty()) g_qr.message = msg;
                else if (st == "scanned") g_qr.message = "已扫码，请在手机端确认登录";
                else if (st == "ok") g_qr.message = "登录成功！";
                else if (st == "expired") g_qr.message = "二维码已过期，请关闭重试";
            }
        }
        --g_active_jobs;
    }).detach();
}

void plugin_qr_close() {
    std::lock_guard<std::mutex> lk(g_mgr_mtx);
    g_qr = PluginQrState{};
}

void plugin_logout(const std::string& plugin_id) {
    std::string spath;
    std::map<std::string, std::string> kv;
    {
        std::lock_guard<std::mutex> lk(g_mgr_mtx);
        PluginEntry* p = find_entry_copy_locked(plugin_id, &spath, &kv);
        if (!p) return;
        p->info.logged_in = false;
        p->info.nickname.clear();
        p->kv.erase("__nickname");
        save_entry_kv(p);
        g_snap.plugins_updated = true;
    }
    ++g_active_jobs;
    std::thread([plugin_id, spath, kv]() {
        PluginEntry local_ent;
        local_ent.info.id = plugin_id;
        local_ent.kv = kv;
        std::string err;
        lua_State* L = create_sandboxed_vm(&local_ent, spath, err);
        if (L) {
            lua_getglobal(L, "plugin");
            lua_getfield(L, -1, "logout");
            if (lua_isfunction(L, -1)) lua_pcall(L, 0, 0, 0);
            lua_close(L);
        }
        if (!g_quit.load()) {
            std::lock_guard<std::mutex> lk(g_mgr_mtx);
            for (auto* p : g_plugins) if (p->info.id == plugin_id) { p->kv = local_ent.kv; save_entry_kv(p); break; }
        }
        --g_active_jobs;
    }).detach();
}

PluginQrState plugin_get_qr_state() {
    std::lock_guard<std::mutex> lk(g_mgr_mtx);
    return g_qr;
}

void plugin_poll_updates(PluginUiSnapshot& dst, bool& out_got_playlists, bool& out_got_tracks, bool& out_got_play) {
    std::lock_guard<std::mutex> lk(g_mgr_mtx);
    out_got_playlists = g_got_playlists;
    out_got_tracks    = g_got_tracks;
    out_got_play      = g_got_play;
    dst.is_busy       = g_snap.is_busy;
    dst.plugins_updated = g_snap.plugins_updated;
    g_snap.plugins_updated = false;
    if (!g_snap.status_toast.empty()) {
        dst.status_toast = std::move(g_snap.status_toast);
        g_snap.status_toast.clear();
    }
    if (g_got_playlists) {
        dst.playlists = g_snap.playlists;
        g_got_playlists = false;
    }
    if (g_got_tracks) {
        dst.tracks = g_snap.tracks;
        dst.active_playlist_name = g_snap.active_playlist_name;
        g_got_tracks = false;
    }
    if (g_got_play) {
        dst.ready_play = std::move(g_snap.ready_play);
        g_snap.ready_play = PluginReadyPlay{};
        g_got_play = false;
    }
}

} // namespace sirene
