#include "spotify/web_api.hpp"

#include <algorithm>
#include <vector>

#include "core/log.hpp"
#include "core/text.hpp"

namespace spotify {

http::Response WebApi::Call(const wchar_t* method, const std::wstring& path,
                            const std::string& jsonBody) {
    if (auth_.AccessToken().empty()) {
        http::Response none;
        none.status = kNoToken;
        return none;
    }

    const std::wstring url = L"https://api.spotify.com/v1" + path;
    auto send = [&] {
        std::vector<std::wstring> headers{L"Authorization: Bearer " +
                                          text::Widen(auth_.AccessToken())};
        if (!jsonBody.empty()) { headers.push_back(L"Content-Type: application/json"); }
        return http::Send(method, url, headers, jsonBody);
    };

    http::Response r = send();
    if (r.status == 401 && auth_.Refresh()) { r = send(); }
    if (r.status == 429) {
        const int wait = std::max(1, r.retryAfterSec);
        backoffUntil_ = GetTickCount64() + static_cast<ULONGLONG>(wait) * 1000;
        logx::Log("[spotify] rate limited, waiting {} s", wait);
    }
    return r;
}

}  // namespace spotify
