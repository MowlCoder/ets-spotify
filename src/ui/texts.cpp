#include "ui/texts.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <format>
#include <string>

#include "core/config.hpp"
#include "core/text.hpp"

namespace texts {
namespace {
// The cab screen is tiny (about 140x60 px at 1080p): keep every line short.
struct Strings {
    const wchar_t* starting;
    std::array<const wchar_t*, 2> noClientId;
    std::array<const wchar_t*, 2> needLogin;  // line: {} = screen key
    std::array<const wchar_t*, 2> loggingIn;
    std::array<const wchar_t*, 2> nothingPlaying;
    const wchar_t* error;
    // Problems ({} = port, HTTP status or screen key)
    const wchar_t* portBusy;
    const wchar_t* loginTimedOut;
    const wchar_t* loginCancelled;
    const wchar_t* loginFailed;
    const wchar_t* noAccess;
    const wchar_t* loginExpired;
    const wchar_t* refreshFailed;
    const wchar_t* offline;
    const wchar_t* httpError;
    // Notices
    const wchar_t* openSpotify;
    const wchar_t* premiumRequired;
    const wchar_t* refused;  // {} = Spotify's reason
    const wchar_t* failed;   // {} = HTTP status
    // Calibration
    std::array<const wchar_t*, 6> calibrationItems;  // in calibration::Item order
    const wchar_t* displayNotFound;
};

// The Russian interface ([ui] language=ru, or auto on a Russian Windows).
constexpr Strings kRussian{
    L"Запуск…",
    {L"Нет client_id", L"см. ets_spotify.ini"},
    {L"Вход в Spotify", L"Нажмите {}"},
    {L"Вход…", L"Войдите в браузере"},
    {L"Тишина", L"Включите Spotify"},
    L"Ошибка",
    L"Порт {} занят",
    L"Время вышло, {} ещё раз",
    L"Вход отменён",
    L"Сбой входа, {} ещё раз",
    L"Нет доступа, см. лог",
    L"Войдите снова: {}",
    L"Доступ не обновился",
    L"Нет связи",
    L"Код {}",
    L"Откройте Spotify",
    L"Нужен Premium",
    L"Отказ: {}",
    L"Не вышло ({})",
    {L"← ЛЕВЫЙ КРАЙ", L"ПРАВЫЙ КРАЙ →", L"↑ ВЕРХНИЙ КРАЙ", L"↓ НИЖНИЙ КРАЙ", L"ПРОПОРЦИИ",
     L"ЭКРАН"},
    L"не найден",
};

constexpr Strings kEnglish{
    L"Starting…",
    {L"No client_id", L"see ets_spotify.ini"},
    {L"Spotify login", L"Press {}"},
    {L"Logging in…", L"Log in in the browser"},
    {L"Silence", L"Start Spotify"},
    L"Error",
    L"Port {} is busy",
    L"Timed out, {} again",
    L"Login cancelled",
    L"Login failed, {} again",
    L"No access, see the log",
    L"Log in again: {}",
    L"Access not renewed",
    L"No connection",
    L"Code {}",
    L"Open Spotify",
    L"Premium needed",
    L"Refused: {}",
    L"Failed ({})",
    {L"← LEFT EDGE", L"RIGHT EDGE →", L"↑ TOP EDGE", L"↓ BOTTOM EDGE", L"PROPORTIONS", L"DISPLAY"},
    L"not found",
};

std::atomic<const Strings*> g_strings{&kRussian};

const Strings& S() { return *g_strings.load(std::memory_order_relaxed); }

// Fills the {} of a pattern (patterns have at most one).
template <typename T>
std::wstring Fill(const wchar_t* pattern, const T& value) {
    return std::vformat(pattern, std::make_wformat_args(value));
}

std::wstring ProblemText(const spotify::State& s, const std::wstring& key) {
    using spotify::Problem;
    switch (s.problem) {
        case Problem::PortBusy: return Fill(S().portBusy, s.code);
        case Problem::LoginTimedOut: return Fill(S().loginTimedOut, key);
        case Problem::LoginCancelled: return S().loginCancelled;
        case Problem::LoginFailed: return Fill(S().loginFailed, key);
        case Problem::NoAccess: return S().noAccess;
        case Problem::LoginExpired: return Fill(S().loginExpired, key);
        case Problem::RefreshFailed: return S().refreshFailed;
        case Problem::Offline: return S().offline;
        case Problem::HttpError: return Fill(S().httpError, s.code);
        case Problem::None: break;
    }
    return std::wstring();
}
}  // namespace

void Reload() {
    const std::wstring language = config::GetString(L"ui", L"language", L"auto");
    bool russian = language == L"ru";
    if (language == L"auto") {
        russian = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_RUSSIAN;
    }
    g_strings.store(russian ? &kRussian : &kEnglish);
}

void Status(const spotify::State& s, const std::wstring& screenKey, std::wstring* title,
            std::wstring* line) {
    using spotify::Status;
    const std::wstring problem = ProblemText(s, screenKey);
    switch (s.status) {
        case Status::Starting:
            *title = L"Spotify";
            *line = S().starting;
            break;
        case Status::NoClientId:
            *title = S().noClientId[0];
            *line = S().noClientId[1];
            break;
        case Status::NeedLogin:
            *title = S().needLogin[0];
            *line = problem.empty() ? Fill(S().needLogin[1], screenKey) : problem;
            break;
        case Status::LoggingIn:
            *title = S().loggingIn[0];
            *line = S().loggingIn[1];
            break;
        case Status::NothingPlaying:
            *title = S().nothingPlaying[0];
            *line = S().nothingPlaying[1];
            break;
        case Status::Error:
            *title = S().error;
            *line = problem;
            break;
        case Status::Track: break;
    }
}

std::wstring Notice(const spotify::State& s) {
    using spotify::Notice;
    switch (s.notice) {
        case Notice::OpenSpotify: return S().openSpotify;
        case Notice::PremiumRequired: return S().premiumRequired;
        case Notice::Refused: return Fill(S().refused, text::Widen(s.noticeDetail));
        case Notice::Failed: return Fill(S().failed, s.noticeCode);
        case Notice::None: break;
    }
    return std::wstring();
}

std::wstring CalibrationLabel(calibration::Item item) {
    const int i = static_cast<int>(item);
    static_assert(static_cast<int>(calibration::Item::Count) == 6,
                  "one label per calibration item");
    return S().calibrationItems[std::clamp(i, 0, 5)];
}

std::wstring DisplayNotFound() { return S().displayNotFound; }
}  // namespace texts
