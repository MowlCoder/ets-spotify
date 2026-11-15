#pragma once
#include <windows.h>

#include <functional>
#include <string>

#include "spotify/spotify.hpp"

namespace spotify {

// Tokens for one client_id. Login is OAuth 2.0 Authorization Code + PKCE (no client secret): the
// browser goes to accounts.spotify.com, which redirects to http://127.0.0.1:<port>/callback, where
// a tiny local server (core/http.hpp) takes the code. The refresh token is kept by
// spotify/token_store.hpp. Used by the Spotify worker thread only.
class Auth {
public:
    // Applies [spotify] settings. A different client_id drops the current login and loads the
    // stored one.
    void Configure(const std::string& clientId, unsigned short port);
    const std::string& ClientId() const { return clientId_; }
    bool HasLogin() const { return !refreshToken_.empty(); }

    // A valid access token, refreshed when it is about to expire; empty when that fails.
    const std::string& AccessToken();
    bool Refresh();  // forces a new access token (after a 401)

    struct LoginResult {
        bool ok = false;
        Problem problem = Problem::None;
        int code = 0;
    };
    // Opens the browser and waits for the redirect (up to 5 minutes, or until `cancel` is
    // signalled). `waiting` runs once the local server listens and the browser has been asked to
    // open.
    LoginResult Login(HANDLE cancel, const std::function<void()>& waiting);

private:
    std::string RedirectUri() const;
    bool TokenRequest(const std::string& form);

    std::string clientId_;
    unsigned short port_ = 8888;
    std::string refreshToken_;
    std::string accessToken_;
    ULONGLONG accessExpiresAt_ = 0;
};

}  // namespace spotify
