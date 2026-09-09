// Playback Machine — null audio sink (headless testing / no sound device).
#pragma once
#include "audiosink.h"

class NullAudioSink : public AudioSink {
public:
    bool open(int) override { return true; }
    void close() override {}
    bool isOpen() const override { return true; }
    bool write(const int16_t *, int) override { return true; }
    long long consumed() const override { return 0; }
    long long pending() const override { return 0; }
    void resetClock() override {}
    void setVolume(int) override {}
    // Not a device: the engine falls back to a free-running wall clock.
    bool isDevice() const override { return false; }
};
