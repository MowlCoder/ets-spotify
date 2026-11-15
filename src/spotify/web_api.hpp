#pragma once
#include <windows.h>

#include <string>

#include "core/http.hpp"
#include "spotify/auth.hpp"

namespace spotify {

// Spotify Web API calls (https://api.spotify.com/v1...) with the access token from Auth: refreshes
// the token once on 401 and remembers Retry-After on 429. Worker thread only.
class WebApi {
public:
    static constexpr int kNoToken = -1;  // Response::status when there is no usable access token

    explicit WebApi(Auth& auth) : auth_(auth) {}
    http::Response Call(const wchar_t* method, const std::wstring& path,
                        const std::string& jsonBody = std::string());
    bool BackingOff() const { return GetTickCount64() < backoffUntil_; }

private:
    Auth& auth_;
    ULONGLONG backoffUntil_ = 0;
};

}  // namespace spotify
