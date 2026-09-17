#include "version_checker.h"
#include <atomic>
#include <cstring>

#pragma comment(lib, "winhttp.lib")
#include <winhttp.h>

extern "C" void Log(const char* fmt, ...);

static constexpr const wchar_t* HOST = L"api.github.com";
static constexpr const wchar_t* PATH = L"/repos/suprepupre/wow-optimize/releases/latest";

static char g_version[32] = {};
static std::atomic<bool> g_ready{false};
static HANDLE g_thread = nullptr;

static DWORD WINAPI CheckerThread(LPVOID) {
    HINTERNET session = WinHttpOpen(L"wow_optimize/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return 0;

    HINTERNET connect = WinHttpConnect(session, HOST, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!connect) {
        WinHttpCloseHandle(session);
        return 0;
    }

    HINTERNET request = WinHttpOpenRequest(connect, L"GET", PATH, NULL, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!request) {
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return 0;
    }

    if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request, NULL)) {
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return 0;
    }

    char buf[4096] = {};
    DWORD total = 0, read = 0;
    while (total < sizeof(buf) - 1 &&
           WinHttpReadData(request, buf + total, sizeof(buf) - 1 - total, &read) && read > 0) {
        total += read;
    }
    buf[total] = '\0';

    const char* key = "\"tag_name\":\"";
    const char* p = strstr(buf, key);
    if (p) {
        p += strlen(key);
        const char* q = strchr(p, '"');
        if (q) {
            size_t n = static_cast<size_t>(q - p);
            if (n > 0 && n < sizeof(g_version)) {
                memcpy(g_version, p, n);
                g_version[n] = '\0';
                g_ready = true;
                Log("[VersionChecker] Latest release: %s", g_version);
            }
        }
    } else {
        Log("[VersionChecker] No tag_name in response");
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return 0;
}

bool VersionChecker_Init() {
    g_ready = false;
    g_version[0] = '\0';
    g_thread = CreateThread(NULL, 0, CheckerThread, NULL, 0, NULL);
    return g_thread != NULL;
}

void VersionChecker_Shutdown() {
    if (g_thread) {
        WaitForSingleObject(g_thread, 3000);
        CloseHandle(g_thread);
        g_thread = nullptr;
    }
}

bool VersionChecker_GetLatestVersion(char* out, size_t outLen) {
    if (!g_ready.load()) return false;
    size_t n = strlen(g_version);
    if (n >= outLen) n = outLen - 1;
    memcpy(out, g_version, n);
    out[n] = '\0';
    return true;
}
