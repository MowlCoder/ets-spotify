#pragma once
#include <array>
#include <windows.h>

#include <cwctype>
#include <string>

// Key names used in [keys] of ets_spotify.ini <-> Windows virtual-key codes.
// Pure code: unit-tested in tests/unit.
namespace keymap {

struct NamedKey {
    const wchar_t* name;
    int vk;
};

inline constexpr auto kNamedKeys = std::to_array<NamedKey>({
    {L"PERIOD", VK_OEM_PERIOD}, {L"COMMA", VK_OEM_COMMA},   {L"MINUS", VK_OEM_MINUS},
    {L"PLUS", VK_OEM_PLUS},     {L"SEMICOLON", VK_OEM_1},   {L"SLASH", VK_OEM_2},
    {L"BACKQUOTE", VK_OEM_3},   {L"LBRACKET", VK_OEM_4},    {L"BACKSLASH", VK_OEM_5},
    {L"RBRACKET", VK_OEM_6},    {L"QUOTE", VK_OEM_7},       {L"INSERT", VK_INSERT},
    {L"DELETE", VK_DELETE},     {L"HOME", VK_HOME},         {L"END", VK_END},
    {L"PAGEUP", VK_PRIOR},      {L"PAGEDOWN", VK_NEXT},     {L"UP", VK_UP},
    {L"DOWN", VK_DOWN},         {L"LEFT", VK_LEFT},         {L"RIGHT", VK_RIGHT},
    {L"PAUSE", VK_PAUSE},       {L"SCROLLLOCK", VK_SCROLL}, {L"MULTIPLY", VK_MULTIPLY},
    {L"ADD", VK_ADD},           {L"SUBTRACT", VK_SUBTRACT}, {L"DECIMAL", VK_DECIMAL},
    {L"DIVIDE", VK_DIVIDE},
});

// "F8", "period", "A", "7", "NUMPAD5"... (case-insensitive). 0 if unknown.
inline int Parse(std::wstring name) {
    for (wchar_t& c : name) { c = static_cast<wchar_t>(std::towupper(c)); }
    if (name.size() == 1 &&
        ((name[0] >= L'A' && name[0] <= L'Z') || (name[0] >= L'0' && name[0] <= L'9'))) {
        return name[0];  // letter and digit keys use their ASCII code
    }

    auto number = [&](const wchar_t* prefix, int low, int high) {
        const size_t n = wcslen(prefix);
        if (name.size() <= n || name.compare(0, n, prefix) != 0) { return -1; }
        int value = 0;
        for (size_t i = n; i < name.size(); ++i) {
            if (name[i] < L'0' || name[i] > L'9') { return -1; }
            value = value * 10 + (name[i] - L'0');
        }
        return value >= low && value <= high ? value : -1;
    };
    if (const int f = number(L"F", 1, 24); f > 0) { return VK_F1 + f - 1; }
    if (const int pad = number(L"NUMPAD", 0, 9); pad >= 0) { return VK_NUMPAD0 + pad; }
    for (const NamedKey& k : kNamedKeys) {
        if (name == k.name) { return k.vk; }
    }
    return 0;
}

// Inverse of Parse(), for the log.
inline std::wstring Name(int vk) {
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) {
        return std::wstring(1, static_cast<wchar_t>(vk));
    }
    if (vk >= VK_F1 && vk <= VK_F24) { return L"F" + std::to_wstring(vk - VK_F1 + 1); }
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) {
        return L"NUMPAD" + std::to_wstring(vk - VK_NUMPAD0);
    }
    for (const NamedKey& k : kNamedKeys) {
        if (vk == k.vk) { return k.name; }
    }
    return L"?";
}

}  // namespace keymap
