#include "spotify/spotify.hpp"

#include <array>
#include <windows.h>
#include <objbase.h>

#include <algorithm>
#include <atomic>
#include <deque>
#include <mutex>

#include "core/config.hpp"
#include "core/immortal.hpp"
#include "core/log.hpp"
#include "core/text.hpp"
#include "core/worker.hpp"
#include "spotify/auth.hpp"
#include "spotify/player.hpp"
#include "spotify/web_api.hpp"
#include "ui/ui_renderer.hpp"

namespace spotify {
namespace {
constexpr DWORD kPollMs = 1000;
constexpr ULONGLONG kNoticeMs = 5000;
constexpr DWORD kAfterCommandMs = 350;  // let Spotify switch tracks before reading the state again
constexpr ULONGLONG kFreshStateMs = 3000;  // play/pause needs a recent is_playing

core::Worker& g_worker = core::Immortal<core::Worker>();
HANDLE g_wakeEvent = nullptr;    // new commands, SetActive(), settings may have changed
HANDLE g_loginCancel = nullptr;  // the screen key during a login abandons it
std::atomic<bool> g_active{false};
std::atomic<bool> g_loginRequested{false};
std::atomic<bool> g_loggingIn{false};

std::mutex& g_mutex = core::Immortal<std::mutex>();  // guards the two below
std::shared_ptr<const State>& g_published =
    core::Immortal<std::shared_ptr<const State>>(std::make_shared<State>());
std::deque<Command>& g_commands = core::Immortal<std::deque<Command>>();

const char* StatusName(Status s) {
    switch (s) {
        case Status::Starting: return "starting";
        case Status::NoClientId: return "no client_id";
        case Status::NeedLogin: return "need login";
        case Status::LoggingIn: return "logging in";
        case Status::NothingPlaying: return "nothing playing";
        case Status::Track: return "track";
        case Status::Error: return "error";
    }
    return "?";
}

const char* ProblemName(Problem p) {
    switch (p) {
        case Problem::None: return "";
        case Problem::PortBusy: return ": port busy";
        case Problem::LoginTimedOut: return ": login timed out";
        case Problem::LoginCancelled: return ": login cancelled";
        case Problem::LoginFailed: return ": login failed (state mismatch)";
        case Problem::NoAccess: return ": token request refused";
        case Problem::LoginExpired: return ": login expired";
        case Problem::RefreshFailed: return ": token refresh failed";
        case Problem::Offline: return ": offline";
        case Problem::HttpError: return ": HTTP error";
    }
    return "";
}

std::shared_ptr<const std::string> DownloadCover(const std::string& url) {
    if (url.empty()) { return nullptr; }
    const http::Response r = http::Send(L"GET", text::Widen(url), {});
    if (r.status != 200 || r.body.empty()) {
        logx::Log("[spotify] cover download failed: HTTP {}", r.status);
        return nullptr;
    }
    return std::make_shared<const std::string>(r.body);
}

bool Ok(const http::Response& r) { return r.status >= 200 && r.status < 300; }

// Everything the worker thread owns.
class Client {
public:
    explicit Client(HANDLE stop) : stop_(stop), api_(auth_) {}

    void Run() {
        ReloadConfig();
        const std::array<HANDLE, 2> events{stop_, g_wakeEvent};
        for (;;) {
            if (auth_.ClientId().empty()) {
                SetStatus(Status::NoClientId);
            } else if (!auth_.HasLogin() && state_.status != Status::NeedLogin &&
                       state_.status != Status::Error) {
                SetStatus(Status::NeedLogin);
            }

            const bool canPoll = g_active.load() && !auth_.ClientId().empty() && auth_.HasLogin();
            const DWORD wait =
                WaitForMultipleObjects(static_cast<DWORD>(events.size()), events.data(), FALSE,
                                       canPoll ? kPollMs : INFINITE);
            if (wait == WAIT_OBJECT_0) { break; }
            if (wait == WAIT_OBJECT_0 + 1) { ReloadConfig(); }

            std::deque<Command> commands;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                commands.swap(g_commands);
            }

            if (auth_.ClientId().empty()) { continue; }
            if (!auth_.HasLogin()) {
                if (g_active.load() && g_loginRequested.exchange(false)) { Login(); }
                continue;
            }
            if (api_.BackingOff()) { continue; }

            for (Command c : commands) { Execute(c); }
            if (g_active.load() && GetTickCount64() - lastPoll_ >= kPollMs - 100) { Poll(); }
        }
    }

private:
    // ---- State shown on the screen ----

    void Publish() {
        auto copy = std::make_shared<State>(state_);
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            copy->version = ++version_;
            g_published = std::move(copy);
        }
        ui::RequestRedraw();
    }

    void SetStatus(Status status, Problem problem = Problem::None, int code = 0) {
        if (state_.status == status && state_.problem == problem && state_.code == code) { return; }
        logx::Log("[spotify] status -> {}{}", StatusName(status), ProblemName(problem));
        state_.status = status;
        state_.problem = problem;
        state_.code = code;
        Publish();
    }

    void ShowNotice(Notice notice, int code = 0, const std::string& detail = std::string()) {
        logx::Log("[spotify] notice {} {} (code {})", static_cast<int>(notice), detail, code);
        state_.notice = notice;
        state_.noticeCode = code;
        state_.noticeDetail = detail;
        state_.noticeUntil = GetTickCount64() + kNoticeMs;
        Publish();
    }

    // Shows a play/pause change at once; the next poll confirms it.
    void SetPlayingNow(bool playing) {
        const ULONGLONG now = GetTickCount64();
        state_.progressMs = ProgressNow(state_, now);
        state_.fetchedAt = now;
        state_.isPlaying = playing;
        Publish();
    }

    // ---- Settings and login ----

    void ReloadConfig() {
        std::string id = text::Narrow(config::GetString(L"spotify", L"client_id", L""));
        id.erase(std::remove_if(id.begin(), id.end(),
                                [](char c) { return c == ' ' || c == '\t' || c == '"'; }),
                 id.end());
        const int port = config::GetInt(L"spotify", L"port", 8888);
        auth_.Configure(id, static_cast<unsigned short>(port > 0 && port < 65536 ? port : 8888));
    }

    void Login() {
        ResetEvent(g_loginCancel);
        const Auth::LoginResult result = auth_.Login(g_loginCancel, [this] {
            g_loggingIn.store(true);
            SetStatus(Status::LoggingIn);
        });
        g_loggingIn.store(false);

        if (result.ok) {
            SetStatus(Status::NothingPlaying);
        } else if (result.problem == Problem::PortBusy || result.problem == Problem::LoginFailed ||
                   result.problem == Problem::NoAccess) {
            SetStatus(Status::Error, result.problem, result.code);
        } else {
            SetStatus(Status::NeedLogin, result.problem);
        }
    }

    // ---- Player ----

    void Poll() {
        lastPoll_ = GetTickCount64();
        const http::Response r = api_.Call(L"GET", L"/me/player?additional_types=episode");

        if (r.status == WebApi::kNoToken) {
            if (auth_.HasLogin()) {
                SetStatus(Status::Error, Problem::RefreshFailed);
            } else {
                SetStatus(Status::NeedLogin, Problem::LoginExpired);
            }
            return;
        }
        if (r.status == 0) {
            if (!offlineLogged_) {
                logx::Log("[spotify] no connection (winhttp error {})", r.error);
            }
            offlineLogged_ = true;
            SetStatus(Status::Error, Problem::Offline);
            return;
        }
        offlineLogged_ = false;
        if (r.status == 429) {
            return;  // keep showing the last state
        }

        PlayerInfo p;
        const bool empty = r.status == 204 || (r.status == 200 && r.body.empty());
        if (!empty && (r.status != 200 || !ParsePlayer(r.body, &p))) {
            logx::Log("[spotify] /me/player: HTTP {} {}", r.status, ErrorReason(r.body));
            SetStatus(Status::Error, Problem::HttpError, r.status);
            return;
        }
        if (empty || !p.hasItem) {
            state_.isPlaying = false;
            SetStatus(Status::NothingPlaying);
            return;
        }

        if (p.coverUrl != state_.coverUrl) {
            state_.coverUrl = p.coverUrl;
            state_.cover = DownloadCover(p.coverUrl);
        }
        if (p.title != state_.title || state_.status != Status::Track) {
            logx::Log("[spotify] now: {} - {}", p.artist, p.title);
        }

        state_.title = p.title;
        state_.artist = p.artist;
        state_.device = p.device;
        state_.isPlaying = p.isPlaying;
        state_.progressMs = p.progressMs;
        state_.durationMs = p.durationMs;
        state_.fetchedAt = GetTickCount64();
        state_.status = Status::Track;
        state_.problem = Problem::None;
        Publish();
    }

    // Moves playback to an available device (the PC app first), e.g. when nothing is active.
    bool TransferPlayback(bool play) {
        const http::Response devices = api_.Call(L"GET", L"/me/player/devices");
        Device device;
        if (devices.status != 200 || !PickDevice(devices.body, &device)) { return false; }

        const std::string body = "{\"device_ids\":[" + json::Quote(device.id) +
                                 "],\"play\":" + (play ? "true" : "false") + "}";
        const http::Response r = api_.Call(L"PUT", L"/me/player", body);
        logx::Log("[spotify] playback moved to '{}': HTTP {}", device.name, r.status);
        return Ok(r);
    }

    // Pause/resume for the cabin (auto-pause). Silent: no notices, no device transfer.
    void RunAutoCommand(Command command) {
        if (command == Command::AutoResume && !autoPaused_) { return; }
        if (GetTickCount64() - lastPoll_ > kPollMs) { Poll(); }
        if (state_.status != Status::Track) {
            autoPaused_ = false;
            return;
        }

        const bool pause = command == Command::AutoPause;
        if (pause && !state_.isPlaying) {
            return;  // already paused (maybe by the player): leave it alone
        }
        autoPaused_ = false;
        if (!pause && state_.isPlaying) {
            return;  // the player resumed it meanwhile
        }

        const http::Response r =
            api_.Call(L"PUT", pause ? L"/me/player/pause" : L"/me/player/play");
        logx::Log("[spotify] auto-{}: HTTP {}", pause ? "pause" : "resume", r.status);
        if (Ok(r)) {
            autoPaused_ = pause;
            SetPlayingNow(!pause);
        }
    }

    void Execute(Command command) {
        if (command == Command::AutoPause || command == Command::AutoResume) {
            RunAutoCommand(command);
            return;
        }
        if (GetTickCount64() - lastPoll_ > kFreshStateMs) { Poll(); }
        if (state_.status == Status::NeedLogin || state_.status == Status::NoClientId) { return; }

        http::Response r;
        if (command == Command::PlayPause) {
            autoPaused_ = false;  // the player takes over
            r = api_.Call(L"PUT", state_.isPlaying ? L"/me/player/pause" : L"/me/player/play");
        } else {
            r = api_.Call(L"POST",
                          command == Command::Next ? L"/me/player/next" : L"/me/player/previous");
        }

        const std::string reason = r.status >= 400 ? ErrorReason(r.body) : std::string();
        if (Ok(r)) {
            if (command == Command::PlayPause) { SetPlayingNow(!state_.isPlaying); }
        } else if (r.status == 404 || reason == "NO_ACTIVE_DEVICE") {
            if (!TransferPlayback(command == Command::PlayPause)) {
                ShowNotice(Notice::OpenSpotify);
            }
        } else if (r.status == 403) {
            if (reason == "PREMIUM_REQUIRED") {
                ShowNotice(Notice::PremiumRequired);
            } else {
                ShowNotice(Notice::Refused, r.status, reason);
            }
        } else if (r.status != 429) {
            logx::Log("[spotify] command failed: HTTP {} {}", r.status, reason);
            ShowNotice(Notice::Failed, r.status);
        }

        if (WaitForSingleObject(stop_, kAfterCommandMs) == WAIT_TIMEOUT) {
            Poll();  // new title and cover
        }
    }

    const HANDLE stop_;
    Auth auth_;
    WebApi api_;
    State state_;
    uint64_t version_ = 0;
    ULONGLONG lastPoll_ = 0;
    bool offlineLogged_ = false;
    bool autoPaused_ = false;  // the cabin paused playback and may resume it
};

void Main(HANDLE stop) {
    const HRESULT coinit =
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);  // ShellExecute
    {
        Client client(stop);
        client.Run();
    }
    if (SUCCEEDED(coinit)) { CoUninitialize(); }
}
}  // namespace

void Start() {
    // The events are never closed: other threads may signal them at any time.
    if (!g_wakeEvent) { g_wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr); }
    if (!g_loginCancel) { g_loginCancel = CreateEventW(nullptr, TRUE, FALSE, nullptr); }
    g_worker.Start(Main);
}

void Stop() {
    if (g_loginCancel) { SetEvent(g_loginCancel); }
    g_worker.Stop(3000);  // a WinHTTP call can take a few seconds to time out
}

void SetActive(bool active) {
    g_active.store(active);
    if (active) { g_loginRequested.store(true); }

    // Any screen key press while the browser login is pending abandons it; with active=true the
    // worker then re-reads ets_spotify.ini (client_id may have changed) and starts a fresh login.
    if (g_loggingIn.load() && g_loginCancel) { SetEvent(g_loginCancel); }
    if (g_wakeEvent) { SetEvent(g_wakeEvent); }
}

void Send(Command command) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_commands.push_back(command);
    }
    if (g_wakeEvent) { SetEvent(g_wakeEvent); }
}

std::shared_ptr<const State> Current() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_published;
}

void PublishForTest(const State& state) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_published = std::make_shared<State>(state);
    }
    ui::RequestRedraw();
}

int ProgressNow(const State& state, unsigned long long now) {
    long long progress = state.progressMs;
    if (state.isPlaying && now > state.fetchedAt) {
        progress += static_cast<long long>(now - state.fetchedAt);
    }
    if (state.durationMs > 0) { progress = std::min<long long>(progress, state.durationMs); }
    return static_cast<int>(std::max<long long>(progress, 0));
}

}  // namespace spotify
