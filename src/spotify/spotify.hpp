#pragma once
#include <cstdint>
#include <memory>
#include <string>

// Spotify Web API client on its own thread.
//
// Playback stays in the official Spotify app (on this PC or the phone): this client only reads the
// player state and sends play/pause/next/previous, so the phone and the PC stay in sync by design.
// Login is OAuth Authorization Code + PKCE through the browser (spotify/auth.hpp). Settings:
// [spotify] client_id and port in ets_spotify.ini.
//
// The client reports what happened as codes (Status, Problem, Notice); ui/texts turns them into
// words.
namespace spotify {

enum class Status {
    Starting,
    NoClientId,      // client_id missing in ets_spotify.ini
    NeedLogin,       // no token yet: the next SetActive(true) opens the browser
    LoggingIn,       // waiting for the browser redirect
    NothingPlaying,  // logged in, no active device or nothing loaded
    Track,           // a track or episode is loaded (playing or paused)
    Error,
};

// Why the player is not shown (with Status::NeedLogin or Status::Error).
enum class Problem {
    None,
    PortBusy,  // code = port
    LoginTimedOut,
    LoginCancelled,
    LoginFailed,    // the browser came back without a valid answer
    NoAccess,       // Spotify refused the token request (see the log)
    LoginExpired,   // the stored login is no longer valid
    RefreshFailed,  // could not get a fresh access token (network?)
    Offline,
    HttpError,  // code = HTTP status
};

// Short-lived hint after a command.
enum class Notice {
    None,
    OpenSpotify,  // no device to play on
    PremiumRequired,
    Refused,  // noticeDetail = Spotify's reason
    Failed,   // noticeCode = HTTP status
};

struct State {
    uint64_t version = 0;
    Status status = Status::Starting;
    Problem problem = Problem::None;
    int code = 0;  // port or HTTP status, see Problem / Notice

    // UTF-8
    std::string title;
    std::string artist;
    std::string device;
    std::string coverUrl;
    bool isPlaying = false;
    int progressMs = 0;
    int durationMs = 0;
    unsigned long long fetchedAt = 0;          // GetTickCount64() of progressMs
    std::shared_ptr<const std::string> cover;  // encoded image bytes (JPEG)

    Notice notice = Notice::None;
    int noticeCode = 0;
    std::string noticeDetail;
    unsigned long long noticeUntil = 0;  // GetTickCount64() when the notice disappears
};

// AutoPause pauses only if something is playing and remembers that; AutoResume resumes only after
// an AutoPause that actually paused (a manual play/pause in between cancels it). Both are silent.
enum class Command { PlayPause, Next, Previous, AutoPause, AutoResume };

void Start();
void Stop();
// Screen on: poll every second (and start the login if needed). Screen off: no polling.
void SetActive(bool active);
void Send(Command command);
std::shared_ptr<const State> Current();
// Playback position now, extrapolated from the last poll.
int ProgressNow(const State& state, unsigned long long now);

// Tests only (tests/harness.cpp): shows this state as if it came from Spotify.
void PublishForTest(const State& state);

}  // namespace spotify
