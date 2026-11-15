#include "spotify/auth.hpp"

#include <string_view>
#include <shellapi.h>

#include "core/http.hpp"
#include "core/json.hpp"
#include "core/log.hpp"
#include "core/pkce.hpp"
#include "core/text.hpp"
#include "spotify/token_store.hpp"

namespace spotify {
namespace {
constexpr const char* kScopes =
    "user-read-playback-state user-modify-playback-state user-read-currently-playing";
constexpr DWORD kLoginTimeoutMs = 5 * 60 * 1000;
constexpr ULONGLONG kRefreshMarginMs = 60 * 1000;

constexpr std::string_view kLoginPage =
    "<!doctype html><meta charset=utf-8><title>ets-spotify</title>"
    "<body style=\"font-family:'Segoe "
    "UI',sans-serif;background:#121212;color:#fff;text-align:center;padding-top:18vh\">"
    "<h1 style=\"color:#1DB954\">Spotify &rarr; Euro Truck Simulator 2</h1>"
    "<p>You can close this tab and return to the game.</p></body>";
}  // namespace

void Auth::Configure(const std::string& clientId, unsigned short port) {
    port_ = port;
    if (clientId == clientId_) { return; }

    clientId_ = clientId;
    accessToken_.clear();
    accessExpiresAt_ = 0;
    refreshToken_ = clientId_.empty() ? std::string() : token_store::Load(clientId_).value_or("");

    if (!clientId_.empty()) {
        logx::Log("[spotify] client_id set ({} chars), stored login: {}", clientId_.size(),
                  HasLogin() ? "yes" : "no");
    }
}

const std::string& Auth::AccessToken() {
    if (accessToken_.empty() || GetTickCount64() + kRefreshMarginMs > accessExpiresAt_) {
        if (!Refresh()) { accessToken_.clear(); }
    }
    return accessToken_;
}

bool Auth::Refresh() {
    if (refreshToken_.empty()) { return false; }
    return TokenRequest(http::FormEncode({{"grant_type", "refresh_token"},
                                          {"refresh_token", refreshToken_},
                                          {"client_id", clientId_}}));
}

std::string Auth::RedirectUri() const {
    return "http://127.0.0.1:" + std::to_string(port_) + "/callback";
}

bool Auth::TokenRequest(const std::string& form) {
    const http::Response r = http::Send(L"POST", L"https://accounts.spotify.com/api/token",
                                        {L"Content-Type: application/x-www-form-urlencoded"}, form);
    json::Value v;
    json::Parse(r.body, &v);

    if (r.status != 200) {
        logx::Log("[spotify] token request failed: HTTP {} (winhttp {}) {} {}", r.status, r.error,
                  v["error"].Str(), v["error_description"].Str());
        if (r.status == 400 && v["error"].Str() == "invalid_grant") {  // revoked or expired login
            refreshToken_.clear();
            token_store::Forget();
        }
        return false;
    }

    accessToken_ = v["access_token"].Str();
    accessExpiresAt_ = GetTickCount64() + static_cast<ULONGLONG>(v["expires_in"].Num(3600) * 1000);

    const std::string refresh = v["refresh_token"].Str();
    if (!refresh.empty() && refresh != refreshToken_) {  // Spotify may rotate it
        refreshToken_ = refresh;
        token_store::Save(clientId_, refreshToken_);
    }
    return !accessToken_.empty();
}

Auth::LoginResult Auth::Login(HANDLE cancel, const std::function<void()>& waiting) {
    LoginResult result;
    http::CallbackServer server;
    std::string error;
    if (!server.Listen(port_, &error)) {  // before the browser opens: a fast redirect must find us
        logx::Log("[spotify] login: {}", error);
        result.problem = Problem::PortBusy;
        result.code = port_;
        return result;
    }

    const std::string verifier = pkce::RandomString(64);
    const std::string state = pkce::RandomString(24);
    const std::string url = "https://accounts.spotify.com/authorize?" +
                            http::FormEncode({{"response_type", "code"},
                                              {"client_id", clientId_},
                                              {"scope", kScopes},
                                              {"redirect_uri", RedirectUri()},
                                              {"code_challenge_method", "S256"},
                                              {"code_challenge", pkce::Challenge(verifier)},
                                              {"state", state}});

    logx::Log("[spotify] opening the browser for login (redirect {})", RedirectUri());
    ShellExecuteW(nullptr, L"open", text::Widen(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    waiting();

    const http::CallbackResult cb = server.Wait("/callback", kLoginPage, kLoginTimeoutMs, cancel);
    if (!cb.ok) {
        logx::Log("[spotify] login: no redirect ({})", cb.error);
        result.problem = cb.error == "timed out" ? Problem::LoginTimedOut : Problem::None;
        return result;
    }
    if (!http::QueryParam(cb.query, "error").empty()) {
        result.problem = Problem::LoginCancelled;
        return result;
    }
    if (http::QueryParam(cb.query, "state") != state) {  // not the answer to our request
        result.problem = Problem::LoginFailed;
        return result;
    }

    const std::string code = http::QueryParam(cb.query, "code");
    if (!TokenRequest(http::FormEncode({{"grant_type", "authorization_code"},
                                        {"code", code},
                                        {"redirect_uri", RedirectUri()},
                                        {"client_id", clientId_},
                                        {"code_verifier", verifier}}))) {
        result.problem = Problem::NoAccess;
        return result;
    }

    token_store::Save(clientId_, refreshToken_);
    logx::Log("[spotify] logged in");
    result.ok = true;
    return result;
}

}  // namespace spotify
