// Playback Machine — playback engine.
//
// Demuxes + decodes with FFmpeg on a dedicated thread, schedules frame
// presentation on a second thread, and drives audio through an AudioSink.
//
// Features:
//   * frame-accurate stepping (forward pops the decode queue, backward pops
//     a decoded-frame history ring; when the history is exhausted the engine
//     re-seeks and re-decodes, so stepping/reverse never dead-end);
//   * speed 0.1x .. 8x (audio is re-clocked at 48000*speed so it stays in
//     pitch-shifted sync; outside 0.25x..4x audio mutes);
//   * true reverse playback (frame history + reverse audio buffer);
//   * audio-master A/V sync when a real audio device is present, with a
//     free-running wall-clock fallback;
//   * optional hardware decoding (d3d11va / cuda / vaapi, auto-fallback).
//
// The engine is framework-free (no GUI dependency) so the same code runs in
// the Windows app and in the headless smoke test.

#pragma once
#include <atomic>
#include <cstdint>
#include <string>

class AudioSink;

struct EngineInfo {
    std::string container;
    std::string videoCodec, audioCodec;
    int width = 0, height = 0;
    double duration = 0.0; // seconds (0 = unknown / live)
    double fps = 0.0;      // 0 = unknown / VFR
    bool hasVideo = false;
    bool hasAudio = false;
    bool hwAccel = false;
    std::string hwName;
};

enum class PlayerState { Stopped, Playing, Paused, Ended, Error };

class PlaybackEngine {
public:
    PlaybackEngine();
    ~PlaybackEngine();
    PlaybackEngine(const PlaybackEngine &) = delete;
    PlaybackEngine &operator=(const PlaybackEngine &) = delete;

    // Load the FFmpeg bindings (idempotent). Returns false + errOut on failure.
    static bool ffLoad(std::string *errOut);

    // Open a file or URL (any format/codec FFmpeg supports) and start playing.
    bool open(const std::string &path, std::string *errOut);
    void close();

    void play();
    void pause();
    void togglePlayPause();
    void seekTo(double seconds);
    void skipBy(double seconds);
    void stepFrame(int dir); // +1 / -1 (pauses playback first)

    void setSpeed(double s);  // magnitude 0.1 .. 8.0
    void setReverse(bool on); // playback direction
    void setLoop(bool on);    // restart from beginning at the end

    bool isReverse() const;

    // ---- thread-safe GUI polling -------------------------------------
    struct Frame {
        const uint8_t *rgba = nullptr; // top-down RGBA
        int w = 0, h = 0;
        double pts = 0.0;
        uint64_t token = 0; // changes when a new frame is displayed
    };
    Frame currentFrame();
    double position();  // seconds
    double duration() const;
    PlayerState state() const;
    double speed() const;
    int direction() const;
    bool isLoop() const;
    const EngineInfo &info() const;
    std::string lastError() const;

    void setAudioSink(AudioSink *sink);
    void setVolume(int percent);

private:
    struct Impl;
    Impl *p_ = nullptr;
};
