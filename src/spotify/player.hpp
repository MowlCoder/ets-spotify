#pragma once
// Reading Spotify Web API answers. Pure code: unit-tested in tests/unit.

#include <string>

#include "core/json.hpp"

namespace spotify {

// GET /me/player
struct PlayerInfo {
    bool hasItem = false;  // false: nothing loaded (an ad, or a device with an empty queue)
    std::string title;
    std::string artist;
    std::string device;
    std::string coverUrl;
    bool isPlaying = false;
    int progressMs = 0;
    int durationMs = 0;
};

// Spotify usually offers covers of 640, 300 and 64 px; the cover on the cab screen is about 200 px,
// so take the smallest one that is at least that big.
inline std::string PickImage(const json::Value& images) {
    std::string best;
    double bestScore = 1e9;
    for (size_t i = 0; i < images.Size(); ++i) {
        const double w = images[i]["width"].Num(300);
        const double score = w >= 200 ? w - 200 : (200 - w) * 4;
        if (score < bestScore) {
            bestScore = score;
            best = images[i]["url"].Str();
        }
    }
    return best;
}

// False for a body that is not JSON.
inline bool ParsePlayer(const std::string& body, PlayerInfo* out) {
    json::Value v;
    if (!json::Parse(body, &v)) { return false; }

    PlayerInfo p;
    const json::Value& item = v["item"];
    p.hasItem = !item.IsNull();
    if (item["type"].Str() == "episode") {
        p.artist = item["show"]["name"].Str();
        p.coverUrl = PickImage(item["images"]);
    } else {
        const json::Value& artists = item["artists"];
        for (size_t i = 0; i < artists.Size(); ++i) {
            if (!p.artist.empty()) { p.artist += ", "; }
            p.artist += artists[i]["name"].Str();
        }
        p.coverUrl = PickImage(item["album"]["images"]);
    }
    p.title = item["name"].Str();
    p.device = v["device"]["name"].Str();
    p.isPlaying = v["is_playing"].Bool();
    p.progressMs = static_cast<int>(v["progress_ms"].Num());
    p.durationMs = static_cast<int>(item["duration_ms"].Num());

    *out = std::move(p);
    return true;
}

// GET /me/player/devices: the device to move playback to, preferring the Spotify app on this PC.
struct Device {
    std::string id;
    std::string name;
};
inline bool PickDevice(const std::string& body, Device* out) {
    json::Value v;
    json::Parse(body, &v);
    const json::Value& devices = v["devices"];

    Device best;
    for (size_t i = 0; i < devices.Size(); ++i) {
        const json::Value& d = devices[i];
        if (d["is_restricted"].Bool()) { continue; }
        const bool computer = d["type"].Str() == "Computer";
        if (best.id.empty() || computer) {
            best = {d["id"].Str(), d["name"].Str()};
            if (computer) { break; }
        }
    }

    if (best.id.empty()) { return false; }
    *out = std::move(best);
    return true;
}

// The reason in an error answer: {"error": {"status": 403, "message": "...", "reason":
// "PREMIUM_REQUIRED"}}.
inline std::string ErrorReason(const std::string& body) {
    json::Value v;
    json::Parse(body, &v);
    const std::string reason = v["error"]["reason"].Str();
    return reason.empty() ? v["error"]["message"].Str() : reason;
}

}  // namespace spotify
