#pragma once
#include <string>

// UTF-8 <-> UTF-16. Strings inside the plugin are UTF-8; Windows APIs take UTF-16.
namespace text {
std::wstring Widen(const std::string& utf8);
std::string Narrow(const std::wstring& wide);
}  // namespace text
