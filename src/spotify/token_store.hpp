#pragma once
#include <optional>
#include <string>

// The refresh token, encrypted with DPAPI (only this Windows user can decrypt it), in
// %APPDATA%\ets-spotify\token.dat, together with the client_id it was issued for.
namespace spotify::token_store {
// Nothing if missing, unreadable or issued for another client_id; the reason goes to the log.
std::optional<std::string> Load(const std::string& clientId);
void Save(const std::string& clientId, const std::string& refreshToken);
void Forget();
}  // namespace spotify::token_store
