#include "ui/ui_renderer.hpp"

#include <array>
#include <windows.h>
#include <objbase.h>

#include <algorithm>
#include <atomic>
#include <mutex>

#include "core/immortal.hpp"
#include "core/worker.hpp"
#include "input/input.hpp"
#include "screen/calibration.hpp"
#include "screen/screen_profile.hpp"
#include "spotify/spotify.hpp"
#include "ui/painter.hpp"
#include "ui/texts.hpp"

namespace ui {
namespace {
constexpr DWORD kProgressRepaintMs = 1000;  // progress bar
constexpr DWORD kMarqueeRepaintMs = 50;     // a line moving back and forth: 20 frames per second

core::Worker& g_worker = core::Immortal<core::Worker>();
HANDLE g_redrawEvent = nullptr;  // never closed: any thread may signal it
std::atomic<bool> g_reloadSettings{true};
std::atomic<bool> g_visible{
    false};  // the picture is on the cab screen: worth repainting on a timer

std::mutex& g_mutex = core::Immortal<std::mutex>();  // guards the two below
std::shared_ptr<const Frame>& g_latest = core::Immortal<std::shared_ptr<const Frame>>();
uint64_t g_version = 0;

void Publish(std::shared_ptr<Frame> frame) {
    std::lock_guard<std::mutex> lock(g_mutex);
    frame->version = ++g_version;
    g_latest = std::move(frame);
}

void Main(HANDLE stop) {
    const HRESULT coinit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    {
        Painter painter;
        if (painter.Init()) {
            const std::array<HANDLE, 2> events{stop, g_redrawEvent};
            screen::Window window = screen::kDefaultWindow;
            for (;;) {
                if (g_reloadSettings.exchange(false)) {
                    window = screen_profile::LoadWindow();
                    texts::Reload();
                }

                const std::shared_ptr<const spotify::State> state = spotify::Current();
                const unsigned long long now = GetTickCount64();
                const calibration::View calibrating = calibration::Current();
                const bool painted =
                    calibrating.active
                        ? painter.PaintCalibration(calibrating)
                        : painter.PaintPlayer(window, *state, now, input::ScreenKeyName());
                if (painted) {
                    if (auto frame = painter.Snapshot()) { Publish(std::move(frame)); }
                }

                // While on screen: repaint every second while a track plays (progress bar), often
                // while a line moves, and when a notice expires. Otherwise only when something
                // changes.
                DWORD timeout = INFINITE;
                if (g_visible.load()) {
                    if (state->status == spotify::Status::Track && state->isPlaying) {
                        timeout = kProgressRepaintMs;
                    }
                    if (painter.Scrolling()) { timeout = kMarqueeRepaintMs; }
                    if (state->noticeUntil > now) {
                        timeout = std::min<DWORD>(
                            timeout, static_cast<DWORD>(state->noticeUntil - now + 50));
                    }
                }

                if (WaitForMultipleObjects(static_cast<DWORD>(events.size()), events.data(), FALSE,
                                           timeout) == WAIT_OBJECT_0) {
                    break;
                }
            }
        } else {
            WaitForSingleObject(stop, INFINITE);
        }
    }
    if (SUCCEEDED(coinit)) { CoUninitialize(); }
}
}  // namespace

void Start() {
    if (!g_redrawEvent) { g_redrawEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr); }
    g_worker.Start(Main);
}

void Stop() { g_worker.Stop(2000); }

void RequestRedraw() {
    if (g_redrawEvent) { SetEvent(g_redrawEvent); }
}

void ReloadSettings() {
    g_reloadSettings.store(true);
    RequestRedraw();
}

void SetVisible(bool visible) {
    if (g_visible.exchange(visible) != visible && visible) { RequestRedraw(); }
}

std::shared_ptr<const Frame> Latest() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_latest;
}
}  // namespace ui
