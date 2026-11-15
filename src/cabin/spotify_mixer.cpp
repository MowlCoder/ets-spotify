#include "cabin/spotify_mixer.hpp"

#include <windows.h>
#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <array>
#include <filesystem>
#include <format>
#include <unordered_map>
#include <vector>

#include "cabin/cabin_logic.hpp"
#include "core/log.hpp"

using Microsoft::WRL::ComPtr;

namespace {
bool IsSpotifyProcess(DWORD pid) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) { return false; }
    std::array<wchar_t, MAX_PATH> path{};
    DWORD size = static_cast<DWORD>(path.size());
    const bool ok = QueryFullProcessImageNameW(process, 0, path.data(), &size) != 0;
    CloseHandle(process);
    return ok &&
           _wcsicmp(std::filesystem::path(path.data()).filename().c_str(), L"Spotify.exe") == 0;
}
}  // namespace

namespace cabin {

struct SpotifyMixer::Impl {
    struct Session {
        std::wstring id;  // session instance id: stable while the session lives
        ComPtr<ISimpleAudioVolume> volume;
        cabin::VolumeTracker tracker;
    };

    ComPtr<IMMDeviceEnumerator> enumerator;
    std::vector<Session> sessions;
    std::unordered_map<DWORD, bool> isSpotify;  // by process id
    ULONGLONG lastEchoLog = 0;

    // Spotify's sessions on one output device, keeping what we know about sessions seen before.
    void Collect(IMMDevice* device, std::vector<Session>* found) {
        ComPtr<IAudioSessionManager2> manager;
        ComPtr<IAudioSessionEnumerator> list;
        if (FAILED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                                    reinterpret_cast<void**>(manager.GetAddressOf()))) ||
            FAILED(manager->GetSessionEnumerator(&list))) {
            return;
        }

        int count = 0;
        list->GetCount(&count);
        for (int i = 0; i < count; ++i) {
            ComPtr<IAudioSessionControl> control;
            ComPtr<IAudioSessionControl2> control2;
            DWORD pid = 0;
            if (FAILED(list->GetSession(i, &control)) || FAILED(control.As(&control2)) ||
                FAILED(control2->GetProcessId(&pid)) || pid == 0) {
                continue;
            }
            const auto known = isSpotify.find(pid);
            if (!(known != isSpotify.end() ? known->second
                                           : (isSpotify[pid] = IsSpotifyProcess(pid)))) {
                continue;
            }

            Session s;
            if (FAILED(control.As(&s.volume))) { continue; }
            LPWSTR id = nullptr;
            if (SUCCEEDED(control2->GetSessionInstanceIdentifier(&id)) && id) {
                s.id = id;
                CoTaskMemFree(id);
            }

            bool seen = false;
            for (const Session& old : sessions) {
                if (old.id == s.id) {
                    s.tracker = old.tracker;
                    seen = true;
                    break;
                }
            }
            if (!seen) {
                float current = 1.0f;
                s.volume->GetMasterVolume(&current);
                s.tracker = cabin::VolumeTracker(current);
                logx::Log("[cabin] Spotify audio session found (mixer volume {:.2f})", current);
            }
            found->push_back(std::move(s));
        }
    }

    // Logs what Observe() concluded about the mixer value the player sees now.
    void Report(cabin::VolumeTracker::Seen seen, float before, float current, float player,
                ULONGLONG now) {
        if (seen == cabin::VolumeTracker::Seen::Foreign) {
            logx::Log(
                "[cabin] Spotify volume changed outside the mod: mixer {:.2f} -> {:.2f}, player "
                "volume now {:.2f}",
                before, current, player);
        } else if (seen == cabin::VolumeTracker::Seen::Echo && now - lastEchoLog >= 10000) {
            lastEchoLog = now;
            logx::Log(
                "[cabin] Spotify brought back an older volume ({:.2f}, Spotify Connect sync), "
                "re-applying",
                current);
        }
    }
};

SpotifyMixer::SpotifyMixer() : impl_(std::make_unique<Impl>()) {}
SpotifyMixer::~SpotifyMixer() = default;

void SpotifyMixer::Refresh() {
    Impl& m = *impl_;
    if (!m.enumerator && FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                                 IID_PPV_ARGS(&m.enumerator)))) {
        return;
    }
    ComPtr<IMMDeviceCollection> devices;
    if (FAILED(m.enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devices))) {
        return;
    }

    UINT count = 0;
    devices->GetCount(&count);
    std::vector<Impl::Session> found;
    for (UINT d = 0; d < count; ++d) {
        ComPtr<IMMDevice> device;
        if (SUCCEEDED(devices->Item(d, &device))) { m.Collect(device.Get(), &found); }
    }
    m.sessions.swap(found);
}

void SpotifyMixer::Apply(float factor, float seenFactor) {
    const ULONGLONG now = GetTickCount64();
    for (Impl::Session& s : impl_->sessions) {
        float current = 0.0f;
        if (SUCCEEDED(s.volume->GetMasterVolume(&current))) {
            const float before = s.tracker.LastApplied();
            const auto seen = s.tracker.Observe(current, seenFactor, now);
            impl_->Report(seen, before, current, s.tracker.Player(), now);
        }

        const float target = s.tracker.Want(factor);
        if (target >= 0.0f && SUCCEEDED(s.volume->SetMasterVolume(target, nullptr))) {
            s.tracker.Applied(target, now);
        }
    }
}

std::string SpotifyMixer::Describe() const {
    if (impl_->sessions.empty()) {
        return " (no Spotify audio session: is Spotify playing on this PC?)";
    }
    std::string out = " (mixer";
    for (const Impl::Session& s : impl_->sessions) {
        float current = -1.0f;
        s.volume->GetMasterVolume(&current);
        out += std::format(" {:.2f}", current);
    }
    return out + ")";
}

}  // namespace cabin
