#include "core/text.hpp"

#include <windows.h>

namespace text {

std::wstring Widen(const std::string& utf8) {
    if (utf8.empty()) { return std::wstring(); }
    const int size = static_cast<int>(utf8.size());
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), size, nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), size, out.data(), n);
    return out;
}

std::string Narrow(const std::wstring& wide) {
    if (wide.empty()) { return std::string(); }
    const int size = static_cast<int>(wide.size());
    const int n = WideCharToMultiByte(CP_UTF8, 0, wide.data(), size, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), size, out.data(), n, nullptr, nullptr);
    return out;
}

}  // namespace text
