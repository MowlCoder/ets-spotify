#include "spotify/token_store.hpp"

#include <windows.h>
#include <shlobj.h>
#include <wincrypt.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <system_error>
#include <vector>

#include "core/json.hpp"
#include "core/log.hpp"

namespace spotify::token_store {
namespace {
std::filesystem::path Path() {
    std::filesystem::path dir;
    PWSTR appdata = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appdata))) {
        dir = std::filesystem::path(appdata) / L"ets-spotify";
        CoTaskMemFree(appdata);
    }

    std::error_code ignored;
    std::filesystem::create_directories(dir, ignored);
    return dir / L"token.dat";
}

std::vector<BYTE> ReadAll(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return std::vector<BYTE>(std::istreambuf_iterator<char>(file),
                             std::istreambuf_iterator<char>());
}

// Frees a buffer that CryptProtectData / CryptUnprotectData allocated.
struct LocalBlob {
    DATA_BLOB blob{};
    LocalBlob() = default;
    LocalBlob(const LocalBlob&) = delete;
    LocalBlob& operator=(const LocalBlob&) = delete;
    ~LocalBlob() {
        if (blob.pbData) { LocalFree(blob.pbData); }
    }
};
}  // namespace

std::optional<std::string> Load(const std::string& clientId) {
    std::vector<BYTE> data = ReadAll(Path());
    if (data.empty()) { return std::nullopt; }  // never logged in: not worth a log line

    DATA_BLOB in{static_cast<DWORD>(data.size()), data.data()};
    LocalBlob out;
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN,
                            &out.blob)) {
        logx::Log("[spotify] stored token could not be decrypted, login needed");
        return std::nullopt;
    }

    const std::string text(reinterpret_cast<const char*>(out.blob.pbData), out.blob.cbData);
    json::Value v;
    if (!json::Parse(text, &v) || v["refresh_token"].Str().empty()) {
        logx::Log("[spotify] stored token is damaged, login needed");
        return std::nullopt;
    }
    if (v["client_id"].Str() != clientId) {
        logx::Log("[spotify] stored token belongs to another client_id, login needed");
        return std::nullopt;
    }
    return v["refresh_token"].Str();
}

void Save(const std::string& clientId, const std::string& refreshToken) {
    std::string text = "{\"client_id\":" + json::Quote(clientId) +
                       ",\"refresh_token\":" + json::Quote(refreshToken) + "}";

    DATA_BLOB in{static_cast<DWORD>(text.size()), reinterpret_cast<BYTE*>(text.data())};
    LocalBlob out;
    if (!CryptProtectData(&in, L"ets-spotify", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN,
                          &out.blob)) {
        logx::Log("[spotify] could not encrypt the token: {}", GetLastError());
        return;
    }

    std::ofstream file(Path(), std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(out.blob.pbData),
               static_cast<std::streamsize>(out.blob.cbData));
}

void Forget() {
    std::error_code ignored;
    std::filesystem::remove(Path(), ignored);
}
}  // namespace spotify::token_store
