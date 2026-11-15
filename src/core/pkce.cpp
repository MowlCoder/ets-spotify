#include "core/pkce.hpp"

#include <string_view>
#include <windows.h>
#include <bcrypt.h>

#include <array>
#include <vector>

namespace pkce {

std::string RandomString(size_t length) {
    constexpr std::string_view kChars =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~";
    constexpr size_t kCount = kChars.size();  // 66
    std::string out;
    out.reserve(length);
    while (out.size() < length) {
        std::array<unsigned char, 64> buf{};
        if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, buf.data(), static_cast<ULONG>(buf.size()),
                                            BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
            break;
        }
        for (unsigned char b : buf) {
            if (b >= (256 / kCount) * kCount) {
                continue;  // rejection sampling: no modulo bias
            }
            out.push_back(kChars[b % kCount]);
            if (out.size() == length) { break; }
        }
    }
    return out;
}

std::string Base64Url(const unsigned char* data, size_t length) {
    constexpr std::string_view kAlphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    size_t i = 0;
    for (; i + 2 < length; i += 3) {
        const unsigned v = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
        out.push_back(kAlphabet[(v >> 18) & 63]);
        out.push_back(kAlphabet[(v >> 12) & 63]);
        out.push_back(kAlphabet[(v >> 6) & 63]);
        out.push_back(kAlphabet[v & 63]);
    }

    if (length - i == 1) {
        const unsigned v = data[i] << 16;
        out.push_back(kAlphabet[(v >> 18) & 63]);
        out.push_back(kAlphabet[(v >> 12) & 63]);
    } else if (length - i == 2) {
        const unsigned v = (data[i] << 16) | (data[i + 1] << 8);
        out.push_back(kAlphabet[(v >> 18) & 63]);
        out.push_back(kAlphabet[(v >> 12) & 63]);
        out.push_back(kAlphabet[(v >> 6) & 63]);
    }
    return out;
}

std::string Challenge(const std::string& verifier) {
    std::array<unsigned char, 32> digest{};
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) {
        return std::string();
    }

    std::string input = verifier;  // BCryptHash takes a non-const buffer
    const NTSTATUS status = BCryptHash(alg, nullptr, 0, reinterpret_cast<PUCHAR>(input.data()),
                                       static_cast<ULONG>(input.size()), digest.data(),
                                       static_cast<ULONG>(digest.size()));
    BCryptCloseAlgorithmProvider(alg, 0);
    if (!BCRYPT_SUCCESS(status)) { return std::string(); }
    return Base64Url(digest.data(), digest.size());
}

}  // namespace pkce
