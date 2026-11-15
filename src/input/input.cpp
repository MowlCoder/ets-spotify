#include "input/input.hpp"

#include <array>
#include <windows.h>

#include <atomic>
#include <string>

#include "core/config.hpp"
#include "core/log.hpp"
#include "core/text.hpp"
#include "core/immortal.hpp"
#include "core/worker.hpp"
#include "diagnostics/capture.hpp"
#include "diagnostics/screen_probe.hpp"
#include "game/game_state.hpp"
#include "input/keymap.hpp"
#include "screen/calibration.hpp"
#include "screen/nav_screen.hpp"
#include "spotify/spotify.hpp"

namespace input {
namespace {
constexpr DWORD kPollMs = 25;
constexpr ULONGLONG kLongPressMs = 800;
constexpr ULONGLONG kRepeatDelayMs = 400;
constexpr ULONGLONG kRepeatEveryMs = 60;

core::Worker& g_worker = core::Immortal<core::Worker>();
std::atomic<int> g_screenKey{VK_F8};  // for ScreenKeyName()

struct Bindings {
    int screen;
    int playPause;
    int next;
    int previous;
};

Bindings LoadBindings() {
    auto key = [](const wchar_t* action, const wchar_t* fallback) {
        const std::wstring name = config::GetString(L"keys", action, fallback);
        if (const int vk = keymap::Parse(name)) { return vk; }
        logx::Log("[input] unknown key '{}' for {} in ets_spotify.ini, using {}",
                  text::Narrow(name), text::Narrow(action), text::Narrow(fallback));
        return keymap::Parse(fallback);
    };
    return {key(L"screen", L"F8"), key(L"play_pause", L"F9"), key(L"next", L"PERIOD"),
            key(L"previous", L"COMMA")};
}

bool GameIsForeground() {
    HWND foreground = GetForegroundWindow();
    DWORD pid = 0;
    if (foreground) { GetWindowThreadProcessId(foreground, &pid); }
    return pid == GetCurrentProcessId();
}

bool Down(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

// One key's state between polls. Keys start as "held", so nothing fires for a key already down.
class Key {
public:
    enum class Hold { None, Short, Long };

    explicit Key(int vk) : vk_(vk) {}

    void Forget() {
        down_ = true;
        since_ = 0;
    }

    bool Pressed() {
        const bool now = Down(vk_);
        const bool edge = now && !down_;
        down_ = now;
        return edge;
    }

    // Fires on press and again while held (to move a calibration edge quickly).
    bool PressedOrRepeated(ULONGLONG t) {
        const bool now = Down(vk_);
        bool fire = false;
        if (now && !down_) {
            fire = true;
            repeatAt_ = t + kRepeatDelayMs;
        } else if (now && repeatAt_ && t >= repeatAt_) {
            fire = true;
            repeatAt_ = t + kRepeatEveryMs;
        }
        if (!now) { repeatAt_ = 0; }
        down_ = now;
        return fire;
    }

    // Short press: fires on release. Long press: fires once, while still held.
    Hold Poll(ULONGLONG t) {
        const bool now = Down(vk_);
        Hold event = Hold::None;
        if (now && !down_) {
            since_ = t;
            longFired_ = false;
        } else if (now && since_ && !longFired_ && t - since_ >= kLongPressMs) {
            longFired_ = true;
            event = Hold::Long;
        } else if (!now && down_ && since_ && !longFired_) {
            event = Hold::Short;
        }
        if (!now) { since_ = 0; }
        down_ = now;
        return event;
    }

private:
    const int vk_;
    bool down_ = true;
    bool longFired_ = false;
    ULONGLONG since_ = 0;  // press time; 0 for a press that started before we looked
    ULONGLONG repeatAt_ = 0;
};

struct Context {
    bool shift;
    bool ctrlOrAlt;  // with Ctrl or Alt the key is meant for the game (Ctrl+F9 is a dev binding in
                     // ETS2)
    bool driving;
    bool calibrating;
    bool debugTools;
};

void OnScreenKey(Key::Hold event, const Context& c) {
    if (event == Key::Hold::None || c.ctrlOrAlt) { return; }
    if (c.shift) {
        if (c.debugTools && event == Key::Hold::Short) { probe::CycleMode(); }
    } else if (c.calibrating) {
        calibration::Finish();  // any press saves and leaves
    } else if (event == Key::Hold::Long) {
        calibration::Begin();
    } else {
        nav::Toggle();
        spotify::SetActive(nav::Enabled());
    }
}

void OnPlayPauseKey(const Context& c) {
    if (c.ctrlOrAlt) { return; }
    if (c.shift) {
        if (c.debugTools) { probe::Step(+1); }
    } else if (c.calibrating) {
        calibration::NextItem();
    } else if (c.driving) {
        spotify::Send(spotify::Command::PlayPause);
    }
}

void OnTrackKey(int direction, const Context& c) {
    if (c.ctrlOrAlt || c.shift) { return; }
    if (c.calibrating) {
        calibration::Adjust(direction);
    } else if (c.driving) {
        spotify::Send(direction > 0 ? spotify::Command::Next : spotify::Command::Previous);
    }
}

void Main(HANDLE stop) {
    const bool debugTools = config::GetInt(L"debug", L"tools", 0) != 0;
    const Bindings b = LoadBindings();
    g_screenKey.store(b.screen);
    logx::Log(
        "[input] keys: {} screen (hold: fit to this truck), {} play/pause, {} next, {} previous{}",
        text::Narrow(keymap::Name(b.screen)), text::Narrow(keymap::Name(b.playPause)),
        text::Narrow(keymap::Name(b.next)), text::Narrow(keymap::Name(b.previous)),
        debugTools ? "; debug tools on" : "");

    Key screen(b.screen);
    Key playPause(b.playPause);
    Key next(b.next);
    Key previous(b.previous);
    Key insert(VK_INSERT);
    Key home(VK_HOME);
    const std::array<Key*, 6> keys{&screen, &playPause, &next, &previous, &insert, &home};

    while (WaitForSingleObject(stop, kPollMs) == WAIT_TIMEOUT) {
        if (!GameIsForeground()) {
            for (Key* k : keys) { k->Forget(); }
            continue;
        }

        const ULONGLONG t = GetTickCount64();
        const Context c{Down(VK_SHIFT), Down(VK_CONTROL) || Down(VK_MENU), !game::Paused(),
                        calibration::Active(), debugTools};

        OnScreenKey(screen.Poll(t), c);
        if (playPause.Pressed()) { OnPlayPauseKey(c); }
        if (c.calibrating ? next.PressedOrRepeated(t) : next.Pressed()) { OnTrackKey(+1, c); }
        if (c.calibrating ? previous.PressedOrRepeated(t) : previous.Pressed()) {
            OnTrackKey(-1, c);
        }
        if (insert.Pressed() && !c.ctrlOrAlt && debugTools) { probe::Confirm(); }
        if (home.Pressed() && !c.ctrlOrAlt && debugTools) { capture::Request(3); }
    }
}
}  // namespace

void Start() { g_worker.Start(Main); }
void Stop() { g_worker.Stop(1000); }
std::wstring ScreenKeyName() { return keymap::Name(g_screenKey.load()); }
}  // namespace input
