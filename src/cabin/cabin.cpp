#include "cabin/cabin.hpp"

#include <windows.h>
#include <objbase.h>

#include <cmath>

#include "cabin/cabin_logic.hpp"
#include "cabin/spotify_mixer.hpp"
#include "core/config.hpp"
#include "core/log.hpp"
#include "core/immortal.hpp"
#include "core/worker.hpp"
#include "game/game_state.hpp"
#include "spotify/spotify.hpp"

namespace cabin {
namespace {
constexpr DWORD kTickMs = 250;
constexpr ULONGLONG kSettingsEveryMs = 5000;
constexpr ULONGLONG kSessionRefreshMs = 5000;
constexpr ULONGLONG kPauseDelayMs = 500;  // ignore very short pauses (quick menu flicks)
constexpr ULONGLONG kResumeDelayMs = 300;
constexpr float kVolumeStep = 0.03f;  // per tick: the full 70% -> 100% swing takes ~2.5 s

core::Worker& g_worker = core::Immortal<core::Worker>();

Settings LoadSettings() {
    Settings s;
    s.pauseOnGamePause = config::GetInt(L"autopause", L"game_pause", 1) != 0;
    s.pauseOnIgnitionOff = config::GetInt(L"autopause", L"ignition_off", 1) != 0;
    s.speedVolume = config::GetInt(L"volume", L"speed_volume", 1) != 0;
    s.standstill = config::GetFloat(L"volume", L"standstill", 0.70f);
    s.fullKmh = config::GetFloat(L"volume", L"full_kmh", 90.0f);
    return s;
}

// Pauses Spotify while the game is paused or the ignition is off, and resumes it afterwards.
class AutoPause {
public:
    void Tick(ULONGLONG now, const Settings& settings) {
        const game::Ignition ignition = game::IgnitionState();
        const bool paused = game::Paused();
        const bool want = WantAutoPause({game::HasDriven(), paused, ignition}, settings);
        if (want != want_) {
            want_ = want;
            since_ = now;
        }

        if (want_ && !pausedByUs_ && now - since_ >= kPauseDelayMs) {
            logx::Log("[cabin] auto-pause: {}", paused ? "game paused" : "ignition off");
            spotify::Send(spotify::Command::AutoPause);
            pausedByUs_ = true;
        } else if (!want_ && pausedByUs_ && now - since_ >= kResumeDelayMs) {
            logx::Log("[cabin] auto-resume");
            spotify::Send(spotify::Command::AutoResume);
            pausedByUs_ = false;
        }
    }

private:
    bool want_ = false;
    bool pausedByUs_ = false;
    ULONGLONG since_ = 0;
};

// Spotify gets louder with speed: the player's volume x SpeedFactor(), through the Windows mixer.
class SpeedVolume {
public:
    void Tick(ULONGLONG now, const Settings& settings) {
        const float kmh = game::SpeedKmh();
        if (!settings.speedVolume || !game::HasDriven() || std::isnan(kmh)) {
            Release();
            return;
        }

        if (now - lastRefresh_ >= kSessionRefreshMs) {
            mixer_.Refresh();
            lastRefresh_ = now;
        }

        const float wanted = SpeedFactor(kmh, settings);
        const float seen = factor_;
        factor_ = Approach(factor_, wanted, kVolumeStep);
        mixer_.Apply(factor_, seen);
        controlling_ = true;

        // Log settled levels (rate-limited), so a test drive shows what the mixer got.
        if (factor_ == wanted && std::fabs(factor_ - logged_) >= 0.05f && now - lastLog_ >= 3000) {
            logx::Log("[cabin] speed volume x{:.2f} at {:.0f} km/h{}", factor_, std::fabs(kmh),
                      mixer_.Describe());
            logged_ = factor_;
            lastLog_ = now;
        }
    }

    // Gives Spotify its own volume back.
    void Release() {
        if (!controlling_) { return; }
        mixer_.Restore(factor_);
        logx::Log("[cabin] speed volume off, player volume restored{}", mixer_.Describe());
        controlling_ = false;
        factor_ = 1.0f;
        logged_ = -1.0f;
    }

private:
    SpotifyMixer mixer_;
    float factor_ = 1.0f;
    float logged_ = -1.0f;
    bool controlling_ = false;
    ULONGLONG lastRefresh_ = 0;
    ULONGLONG lastLog_ = 0;
};

void Main(HANDLE stop) {
    const HRESULT coinit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    {
        AutoPause autoPause;
        SpeedVolume speedVolume;
        Settings settings = LoadSettings();
        ULONGLONG settingsAt = GetTickCount64();

        while (WaitForSingleObject(stop, kTickMs) == WAIT_TIMEOUT) {
            const ULONGLONG now = GetTickCount64();
            if (now - settingsAt >= kSettingsEveryMs) {
                settings = LoadSettings();
                settingsAt = now;
            }
            autoPause.Tick(now, settings);
            speedVolume.Tick(now, settings);
        }

        speedVolume.Release();
    }  // COM objects go before CoUninitialize
    if (SUCCEEDED(coinit)) { CoUninitialize(); }
}
}  // namespace

void Start() { g_worker.Start(Main); }
void Stop() { g_worker.Stop(2000); }  // the thread restores Spotify's volume on its way out
}  // namespace cabin
