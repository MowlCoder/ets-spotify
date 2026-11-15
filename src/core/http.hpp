#pragma once
#include <winsock2.h>
#include <windows.h>

#include <string>
#include <string_view>
#include <utility>
#include <vector>

// HTTPS client (WinHTTP) and the tiny local HTTP server that receives the OAuth redirect.
namespace http {

struct Response {
    int status = 0;         // 0 = transport error (see error)
    std::string body;       // raw bytes
    int retryAfterSec = 0;  // from Retry-After (429)
    DWORD error = 0;        // GetLastError() of the failing WinHTTP call
};

// headers: "Name: value" lines without CRLF.
Response Send(const wchar_t* method, const std::wstring& url,
              const std::vector<std::wstring>& headers, const std::string& body = std::string());

std::string UrlEncode(const std::string& utf8);
std::string UrlDecode(const std::string& text);
std::string FormEncode(const std::vector<std::pair<std::string, std::string>>& fields);

// Value of `key` in "a=1&b=2" (URL-decoded), empty if absent.
std::string QueryParam(const std::string& query, const std::string& key);

struct CallbackResult {
    bool ok = false;    // a request for the callback path arrived
    std::string query;  // its query string, e.g. "code=...&state=..."
    std::string error;  // why nothing arrived (timeout, cancelled, socket error)
};

// Listens on 127.0.0.1:port. Listen() must succeed before the browser is opened, otherwise a fast
// redirect can hit a closed port.
class CallbackServer {
public:
    CallbackServer() = default;
    ~CallbackServer();
    CallbackServer(const CallbackServer&) = delete;
    CallbackServer& operator=(const CallbackServer&) = delete;

    bool Listen(unsigned short port, std::string* error);
    // Serves requests until one for `path` arrives (answering it with `page`), the timeout passes
    // or `cancel` is signalled. Other paths (favicon...) get 404.
    CallbackResult Wait(std::string_view path, std::string_view pageHtml, DWORD timeoutMs,
                        HANDLE cancel);

private:
    SOCKET socket_ = INVALID_SOCKET;
    bool wsaStarted_ = false;
};

}  // namespace http
