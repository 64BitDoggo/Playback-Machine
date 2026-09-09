// Playback Machine — audio output abstraction.
//
// The engine feeds the sink with 16-bit stereo *interleaved* PCM at a fixed
// 48 kHz base rate.  The device is opened at 48000 * speed Hz, so changing
// playback speed (0.25x .. 4x) comes for free: the same samples are played
// faster or slower.  Outside that range the engine mutes audio.

#pragma once
#include <cstdint>

class AudioSink {
public:
    virtual ~AudioSink() = default;

    // Open the device at the given output rate (stereo, 16-bit).
    virtual bool open(int sampleRateHz) = 0;
    virtual void close() = 0;
    virtual bool isOpen() const = 0;

    // Non-blocking enqueue of nFrames stereo frames (2 samples each).
    // Returns false if the sink dropped data (queue full).
    virtual bool write(const int16_t *frames, int nFrames) = 0;

    // Frames played back since the last resetClock() — real data only,
    // silence padding is NOT counted (so the clock freezes when paused).
    virtual long long consumed() const = 0;
    // Frames enqueued (or in flight) but not yet played.
    virtual long long pending() const = 0;
    virtual void resetClock() = 0;

    virtual void setVolume(int percent) = 0;

    // True for a real output device (engine may trust its clock for A/V sync).
    virtual bool isDevice() const { return false; }
};
