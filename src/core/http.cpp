#include "core/http.hpp"

#include <string_view>
#include <ws2tcpip.h>
#include <winhttp.h>

#include <array>
#include <cstdlib>
#include <format>
#include <string>

#include "core/version.hpp"

namespace http {
namespace {
HINTERNET Session() {
    // One session for the life of the process (never closed: WinHTTP must not be torn down from
    // DllMain). WinHTTP sessions are thread-safe.
    static const HINTERNET session = [] {
        HINTERNET s = WinHttpOpen(core::kUserAgent.data(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (s) { WinHttpSetTimeouts(s, 5000, 5000, 10000, 15000); }
        return s;
    }();
    return session;
}

struct Handle {
    HINTERNET h = nullptr;
    ~Handle() {
        if (h) { WinHttpCloseHandle(h); }
    }
};
}  // namespace

std::string UrlEncode(const std::string& utf8) {
    constexpr std::string_view kHex = "0123456789ABCDEF";
    std::string out;
    for (const unsigned char c : utf8) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 15]);
        }
    }
    return out;
}

std::string UrlDecode(const std::string& text) {
    std::string out;
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '+') {
            out.push_back(' ');
        } else if (c == '%' && i + 2 < text.size()) {
            const std::string hex = text.substr(i + 1, 2);
            char* end = nullptr;
            const long v = strtol(hex.c_str(), &end, 16);
            if (end == hex.c_str() + 2) {
                out.push_back(static_cast<char>(v));
                i += 2;
            } else {
                out.push_back(c);
            }
        } else {
            out.push_back(c);
        }
    }
    return out;
}

std::string FormEncode(const std::vector<std::pair<std::string, std::string>>& fields) {
    std::string out;
    for (const auto& [key, value] : fields) {
        if (!out.empty()) { out.push_back('&'); }
        out += UrlEncode(key) + "=" + UrlEncode(value);
    }
    return out;
}

std::string QueryParam(const std::string& query, const std::string& key) {
    size_t pos = 0;
    while (pos <= query.size()) {
        const size_t amp = query.find('&', pos);
        const std::string pair =
            query.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
        const size_t eq = pair.find('=');
        if (UrlDecode(pair.substr(0, eq)) == key) {
            return eq == std::string::npos ? std::string() : UrlDecode(pair.substr(eq + 1));
        }
        if (amp == std::string::npos) { break; }
        pos = amp + 1;
    }
    return std::string();
}

Response Send(const wchar_t* method, const std::wstring& url,
              const std::vector<std::wstring>& headers, const std::string& body) {
    Response r;
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    std::array<wchar_t, 256> host{};
    std::array<wchar_t, 2048> path{};
    std::array<wchar_t, 2048> extra{};
    parts.lpszHostName = host.data();
    parts.dwHostNameLength = static_cast<DWORD>(host.size());
    parts.lpszUrlPath = path.data();
    parts.dwUrlPathLength = static_cast<DWORD>(path.size());
    parts.lpszExtraInfo = extra.data();
    parts.dwExtraInfoLength = static_cast<DWORD>(extra.size());
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts)) {
        r.error = GetLastError();
        return r;
    }
    const std::wstring target = std::wstring(path.data()) + extra.data();

    HINTERNET session = Session();
    if (!session) {
        r.error = GetLastError();
        return r;
    }
    Handle connection{WinHttpConnect(session, host.data(), parts.nPort, 0)};
    if (!connection.h) {
        r.error = GetLastError();
        return r;
    }
    const DWORD flags = parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    Handle request{WinHttpOpenRequest(connection.h, method, target.c_str(), nullptr,
                                      WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags)};
    if (!request.h) {
        r.error = GetLastError();
        return r;
    }

    std::wstring headerBlock;
    for (const std::wstring& h : headers) { headerBlock += h + L"\r\n"; }
    const BOOL sent = WinHttpSendRequest(
        request.h, headerBlock.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headerBlock.c_str(),
        headerBlock.empty() ? 0 : static_cast<DWORD>(-1L),
        body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()),
        static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0);
    if (!sent || !WinHttpReceiveResponse(request.h, nullptr)) {
        r.error = GetLastError();
        return r;
    }

    DWORD status = 0;
    DWORD size = sizeof(status);
    WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
    r.status = static_cast<int>(status);

    DWORD retry = 0;
    size = sizeof(retry);
    if (WinHttpQueryHeaders(request.h, WINHTTP_QUERY_RETRY_AFTER | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &retry, &size, WINHTTP_NO_HEADER_INDEX)) {
        r.retryAfterSec = static_cast<int>(retry);
    }

    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.h, &available) || available == 0) { break; }
        const size_t offset = r.body.size();
        r.body.resize(offset + available);
        DWORD read = 0;
        if (!WinHttpReadData(request.h, r.body.data() + offset, available, &read)) {
            r.body.resize(offset);
            break;
        }
        r.body.resize(offset + read);
    }
    return r;
}

CallbackServer::~CallbackServer() {
    if (socket_ != INVALID_SOCKET) { closesocket(socket_); }
    if (wsaStarted_) { WSACleanup(); }
}

bool CallbackServer::Listen(unsigned short port, std::string* error) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        *error = "WSAStartup failed";
        return false;
    }
    wsaStarted_ = true;

    socket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket_ == INVALID_SOCKET) {
        *error = "socket() failed";
        return false;
    }
    BOOL exclusive = TRUE;
    setsockopt(socket_, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive),
               sizeof(exclusive));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (bind(socket_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        listen(socket_, 4) != 0) {
        *error = std::format("port {} is busy (error {})", port, WSAGetLastError());
        return false;
    }
    return true;
}

CallbackResult CallbackServer::Wait(std::string_view path, std::string_view pageHtml,
                                    DWORD timeoutMs, HANDLE cancel) {
    CallbackResult result;
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    while (GetTickCount64() < deadline) {
        if (cancel && WaitForSingleObject(cancel, 0) == WAIT_OBJECT_0) {
            result.error = "cancelled";
            return result;
        }

        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(socket_, &readable);
        timeval tv{0, 250 * 1000};
        if (select(0, &readable, nullptr, nullptr, &tv) <= 0) { continue; }

        SOCKET client = accept(socket_, nullptr, nullptr);
        if (client == INVALID_SOCKET) { continue; }
        DWORD recvTimeout = 3000;
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&recvTimeout),
                   sizeof(recvTimeout));

        std::string request;
        std::array<char, 2048> buf{};
        while (request.find("\r\n\r\n") == std::string::npos && request.size() < 16384) {
            const int n = recv(client, buf.data(), static_cast<int>(buf.size()), 0);
            if (n <= 0) { break; }
            request.append(buf.data(), static_cast<size_t>(n));
        }

        // Request line: "GET /callback?code=...&state=... HTTP/1.1"
        std::string target;
        const size_t sp1 = request.find(' ');
        const size_t sp2 =
            sp1 == std::string::npos ? std::string::npos : request.find(' ', sp1 + 1);
        if (sp2 != std::string::npos) { target = request.substr(sp1 + 1, sp2 - sp1 - 1); }
        const size_t q = target.find('?');
        const std::string targetPath = target.substr(0, q);

        const bool match = targetPath == path;
        const std::string_view html = match ? pageHtml : std::string_view("Not found");
        std::string response =
            std::string(match ? "HTTP/1.1 200 OK\r\n" : "HTTP/1.1 404 Not Found\r\n") +
            "Content-Type: text/html; charset=utf-8\r\nConnection: close\r\nContent-Length: " +
            std::to_string(html.size()) + "\r\n\r\n" + std::string(html);
        send(client, response.data(), static_cast<int>(response.size()), 0);
        shutdown(client, SD_SEND);
        closesocket(client);

        if (match) {
            result.ok = true;
            result.query = q == std::string::npos ? std::string() : target.substr(q + 1);
            return result;
        }
    }
    result.error = "timed out";
    return result;
}

}  // namespace http
