#pragma once
#include <string>

// OAuth 2.0 PKCE helpers (RFC 7636), using the Windows CNG crypto API.
namespace pkce {
// Cryptographically random string of URL-safe "unreserved" characters.
std::string RandomString(size_t length);
// BASE64URL(SHA256(verifier)) without padding.
std::string Challenge(const std::string& verifier);
std::string Base64Url(const unsigned char* data, size_t length);
}  // namespace pkce
