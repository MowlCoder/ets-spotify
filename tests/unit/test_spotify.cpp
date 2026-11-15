// spotify/player.hpp: reading Web API answers.

#include "check.hpp"
#include "spotify/player.hpp"

using test::Check;

TEST_CASE("spotify answers", SpotifyAnswers) {
    using namespace spotify;
    PlayerInfo p;
    Check(ParsePlayer(R"({"is_playing":true,"progress_ms":1500,"device":{"name":"PC"},
        "item":{"type":"track","name":"Song","duration_ms":200000,"artists":[{"name":"A"},{"name":"B"}],
        "album":{"images":[{"url":"u640","width":640},{"url":"u300","width":300},{"url":"u64","width":64}]}}})",
                      &p) &&
              p.hasItem && p.title == "Song" && p.artist == "A, B" && p.device == "PC" &&
              p.isPlaying && p.progressMs == 1500 && p.durationMs == 200000,
          "track: title, artists, device, position");
    Check(p.coverUrl == "u300", "cover: the smallest one of at least 200 px");

    Check(ParsePlayer(
              R"({"is_playing":false,"item":{"type":"episode","name":"Ep","show":{"name":"Podcast"},
        "images":[{"url":"e640","width":640}]}})",
              &p) &&
              p.artist == "Podcast" && p.coverUrl == "e640" && !p.isPlaying,
          "episode: the show is the artist");
    Check(
        ParsePlayer(R"({"is_playing":false,"item":null,"device":{"name":"PC"}})", &p) && !p.hasItem,
        "nothing loaded (an ad)");
    Check(!ParsePlayer("<html>", &p), "not JSON");

    Device d;
    Check(PickDevice(R"({"devices":[{"id":"p","name":"Phone","type":"Smartphone"},
        {"id":"t","name":"TV","type":"TV","is_restricted":true},{"id":"c","name":"PC","type":"Computer"}]})",
                     &d) &&
              d.id == "c",
          "playback moves to the PC app first");
    Check(PickDevice(R"({"devices":[{"id":"p","name":"Phone","type":"Smartphone"}]})", &d) &&
              d.id == "p",
          "otherwise any free device");
    Check(!PickDevice(R"({"devices":[{"id":"t","type":"TV","is_restricted":true}]})", &d),
          "restricted only: none");

    Check(
        ErrorReason(
            R"({"error":{"status":403,"message":"Player command failed","reason":"PREMIUM_REQUIRED"}})") ==
            "PREMIUM_REQUIRED",
        "error reason");
    Check(ErrorReason(R"({"error":{"status":404,"message":"Device not found"}})") ==
              "Device not found",
          "error message when there is no reason");
}
