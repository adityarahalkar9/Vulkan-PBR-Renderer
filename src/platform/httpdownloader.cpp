#include "platform/httpdownloader.hpp"

#include "core/log.hpp"

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#endif

#include <algorithm>

namespace http {

    std::string fileNameFromUrl(const std::string& url) {
        size_t slash = url.find_last_of('/');
        std::string name = slash == std::string::npos ? url : url.substr(slash + 1);
        size_t q = name.find_first_of("?#");
        if(q != std::string::npos) name = name.substr(0, q);
        if(name.empty()) name = "download.glb";
        return name;
    }

#ifdef _WIN32

    static std::wstring toWide(const std::string& s) {
        int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
        std::wstring w(size_t(len > 0 ? len - 1 : 0), L'\0');
        if(len > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), len);
        return w;
    }

    bool downloadToMemory(const std::string& url, std::vector<uint8_t>& out,
                          const ProgressFn& progress, std::string& error) {
        out.clear();

        URL_COMPONENTS parts{};
        parts.dwStructSize = sizeof(parts);
        wchar_t host[256]{};
        wchar_t path[2048]{};
        parts.lpszHostName = host;
        parts.dwHostNameLength = 255;
        parts.lpszUrlPath = path;
        parts.dwUrlPathLength = 2047;

        if(!WinHttpCrackUrl(toWide(url).c_str(), 0, 0, &parts)) {
            error = "invalid URL";
            return false;
        }
        bool secure = (parts.nScheme == INTERNET_SCHEME_HTTPS);

        HINTERNET session = WinHttpOpen(L"PbrRenderer/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if(!session) { error = "WinHttpOpen failed"; return false; }
        WinHttpSetTimeouts(session, 5000, 5000, 30000, 60000);

        HINTERNET connect = WinHttpConnect(session, host, parts.nPort, 0);
        if(!connect) { WinHttpCloseHandle(session); error = "WinHttpConnect failed"; return false; }

        HINTERNET request = WinHttpOpenRequest(connect, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                               WINHTTP_DEFAULT_ACCEPTABLE_TYPES,
                                               secure ? WINHTTP_FLAG_SECURE : 0);
        if(!request) {
            WinHttpCloseHandle(connect);
            WinHttpCloseHandle(session);
            error = "WinHttpOpenRequest failed";
            return false;
        }

        bool ok = false;
        if(WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
           WinHttpReceiveResponse(request, nullptr)) {
            DWORD status = 0, size = sizeof(status);
            WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
            if(status != 200) {
                error = "HTTP status " + std::to_string(status);
            }
            else {
                size_t total = 0;
                ULARGE_INTEGER contentLength{};
                DWORD clSize = sizeof(contentLength);
                if(WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                                       WINHTTP_HEADER_NAME_BY_INDEX, &contentLength, &clSize, WINHTTP_NO_HEADER_INDEX))
                    total = static_cast<size_t>(contentLength.QuadPart);

                uint8_t buffer[65536];
                DWORD avail = 0, read = 0;
                size_t done = 0;
                while(WinHttpQueryDataAvailable(request, &avail) && avail > 0) {
                    DWORD toRead = std::min<DWORD>(avail, sizeof(buffer));
                    if(!WinHttpReadData(request, buffer, toRead, &read) || read == 0) break;
                    out.insert(out.end(), buffer, buffer + read);
                    done += read;
                    if(progress) progress(done, total);
                }
                ok = !out.empty();
                if(!ok) error = "empty response body";
            }
        }
        else {
            error = "request failed (network error or blocked TLS)";
        }

        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        if(ok) LOG_INFO("downloaded {} bytes from {}", out.size(), url);
        return ok;
    }

#else

    bool downloadToMemory(const std::string&, std::vector<uint8_t>&, const ProgressFn&, std::string& error) {
        error = "HTTP downloads are only supported on Windows in this build";
        return false;
    }

#endif

} // namespace http