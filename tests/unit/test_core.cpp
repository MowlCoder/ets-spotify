// core/: JSON reader, PKCE, URL helpers, the OAuth callback listener.

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <array>
#include <string>
#include <thread>

#include "check.hpp"
#include "core/http.hpp"
#include "core/json.hpp"
#include "core/pkce.hpp"
#include "core/text.hpp"

using test::Check;

namespace {
// Minimal HTTP client for the listener test: one request, returns the status line.
std::string Get(unsigned short port, const std::string& target) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        closesocket(s);
        return "connect failed";
    }

    const std::string request = "GET " + target + " HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
    send(s, request.data(), static_cast<int>(request.size()), 0);

    std::string response;
    std::array<char, 1024> buf{};
    int n;
    while ((n = recv(s, buf.data(), static_cast<int>(buf.size()), 0)) > 0) {
        response.append(buf.data(), static_cast<size_t>(n));
    }
    closesocket(s);
    return response.substr(0, response.find("\r\n"));
}
}  // namespace

TEST_CASE("json", Json) {
    json::Value v;
    const bool ok = json::Parse(
        R"({"is_playing":true,"progress_ms":81234,"item":{"name":"\u041f\u0435\u0441\u043d\u044f \ud83c\udfb5",
        "artists":[{"name":"A"},{"name":"B \"quoted\""}],"album":{"images":[{"url":"u640","width":640},{"url":"u300","width":300}]},
        "duration_ms":227000},"device":null,"list":[1,-2.5e2,false]})",
        &v);
    Check(ok, "parses a player-like object");
    Check(v["is_playing"].Bool() && v["progress_ms"].Num() == 81234, "bool and number fields");
    Check(v["item"]["name"].Str() == "\xD0\x9F\xD0\xB5\xD1\x81\xD0\xBD\xD1\x8F \xF0\x9F\x8E\xB5",
          "\\u escapes incl. surrogate pair -> UTF-8");
    Check(
        v["item"]["artists"].Size() == 2 && v["item"]["artists"][1]["name"].Str() == "B \"quoted\"",
        "array of objects, escaped quotes");
    Check(v["item"]["album"]["images"][1]["url"].Str() == "u300", "nested lookup");
    Check(v["device"].IsNull() && v["missing"]["deeper"][3].IsNull(),
          "null and missing keys are safe");
    Check(v["list"][1].Num() == -250 && !v["list"][2].Bool(true), "exponent number, false");

    json::Value bad;
    Check(
        !json::Parse("{\"a\":}", &bad) && !json::Parse("[1,2", &bad) && !json::Parse("{} x", &bad),
        "rejects broken JSON");

    Check(json::Quote("a\"b\\c\n") == "\"a\\\"b\\\\c\\n\"", "Quote escapes");
}

TEST_CASE("pkce", Pkce) {
    // RFC 7636, appendix B.
    Check(pkce::Challenge("dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk") ==
              "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM",
          "S256 challenge matches the RFC example");

    const std::string a = pkce::RandomString(64);
    const std::string b = pkce::RandomString(64);
    bool charset = true;
    for (char c : a) {
        charset &= (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                   c == '-' || c == '.' || c == '_' || c == '~';
    }
    Check(a.size() == 64 && a != b && charset,
          "random verifier: 64 unreserved chars, differs each time");
}

TEST_CASE("url and text", UrlAndText) {
    Check(http::UrlEncode("a b&c=d/\xC3\xA9") == "a%20b%26c%3Dd%2F%C3%A9", "UrlEncode");
    Check(http::UrlDecode("a%20b+c%2F") == "a b c/", "UrlDecode");
    Check(http::FormEncode({{"grant_type", "authorization_code"},
                            {"redirect_uri", "http://127.0.0.1:8888/callback"}}) ==
              "grant_type=authorization_code&redirect_uri=http%3A%2F%2F127.0.0.1%3A8888%2Fcallback",
          "FormEncode");
    Check(http::QueryParam("code=abc%2Fdef&state=xyz", "code") == "abc/def" &&
              http::QueryParam("code=abc&state=xyz", "state") == "xyz" &&
              http::QueryParam("code=abc", "state").empty(),
          "QueryParam");

    const std::string utf8 =
        "\xD0\xA2\xD0\xB8 \xE2\x99\xAA \xF0\x9F\x8E\xB5";  // Cyrillic, a symbol, an emoji
    Check(text::Narrow(text::Widen(utf8)) == utf8 && text::Widen("\xD1\x91") == L"\u0451",
          "UTF-8 <-> UTF-16");
}

TEST_CASE("oauth callback listener", CallbackListener) {
    constexpr unsigned short kPort = 18888;
    http::CallbackServer server;
    std::string error;
    Check(server.Listen(kPort, &error), "listens on 127.0.0.1");

    std::string faviconStatus;
    std::string callbackStatus;
    std::thread browser([&] {
        Sleep(100);
        faviconStatus = Get(kPort, "/favicon.ico");
        callbackStatus = Get(kPort, "/callback?code=abc%2Fdef&state=xyz");
    });
    const http::CallbackResult result = server.Wait("/callback", "<p>ok</p>", 5000, nullptr);
    browser.join();
    Check(faviconStatus == "HTTP/1.1 404 Not Found", "other paths get 404 and do not end the wait");
    Check(callbackStatus == "HTTP/1.1 200 OK", "callback answered with the page");
    Check(result.ok && http::QueryParam(result.query, "code") == "abc/def" &&
              http::QueryParam(result.query, "state") == "xyz",
          "code and state extracted");

    http::CallbackServer second;
    Check(!second.Listen(kPort, &error) && !error.empty(),
          "busy port reported, not silently ignored");

    HANDLE cancel = CreateEventW(nullptr, TRUE, TRUE, nullptr);
    http::CallbackServer third;
    third.Listen(kPort + 1, &error);
    const http::CallbackResult cancelled = third.Wait("/callback", "", 5000, cancel);
    Check(!cancelled.ok && cancelled.error == "cancelled", "wait can be cancelled");
    if (cancel) { CloseHandle(cancel); }
}
