// Playback Machine — Windows waveOut audio sink.
//
// A small fixed pool of WAVEHDR buffers keeps the device continuously fed.
// The multimedia callback recycles buffers, counting only real (non-silence)
// frames into the consumption clock so it freezes while paused.

#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>

#include <atomic>
#include <deque>
#include <mutex>
#include <vector>
#include "audiosink.h"

#ifdef _MSC_VER
#pragma comment(lib, "winmm.lib")
#endif

class WaveOutSink : public AudioSink {
public:
    ~WaveOutSink() override { close(); }

    bool open(int sampleRateHz) override {
        close();
        rate_ = sampleRateHz;
        WAVEFORMATEX fmt{};
        fmt.wFormatTag = WAVE_FORMAT_PCM;
        fmt.nChannels = 2;
        fmt.nSamplesPerSec = (UINT)sampleRateHz;
        fmt.wBitsPerSample = 16;
        fmt.nBlockAlign = 4;
        fmt.nAvgBytesPerSec = (UINT)(sampleRateHz * 4);
        MMRESULT rr = waveOutOpen(&hw_, 0, &fmt, (DWORD_PTR)&WaveOutSink::callback, (DWORD_PTR)this, CALLBACK_FUNCTION);
        if (rr != MMSYSERR_NOERROR)
            return false;
        {
            std::lock_guard<std::mutex> lk(mx_);
            open_ = true;
            consumed_ = 0;
            pending_ = 0;
            ring_.clear();
            inFlight_ = 0;
            // Prime the device so the callback starts circulating.
            for (int i = 0; i < kPrime; ++i)
                primeZeroLocked();
        }
        return true;
    }

    void close() override {
        bool wasOpen = false;
        {
            std::lock_guard<std::mutex> lk(mx_);
            wasOpen = open_;
            open_ = false;
        }
        if (!wasOpen)
            return;
        waveOutReset(hw_);
        waveOutClose(hw_);
        hw_ = nullptr;
        // Free any headers whose completion callback never fired.
        std::lock_guard<std::mutex> lk(mx_);
        for (WaveHdr *wh : pool_)
            delete wh;
        pool_.clear();
    }

    bool isOpen() const override { return open_; }

    bool write(const int16_t *frames, int nFrames) override {
        if (nFrames <= 0)
            return true;
        std::lock_guard<std::mutex> lk(mx_);
        if (!open_)
            return false;
        ring_.insert(ring_.end(), frames, frames + nFrames * 2);
        pending_ += nFrames;
        // Cap the ring.
        long long maxFrames = (long long)kMaxChunks * kChunkFrames;
        long long have = ring_.size() / 2;
        if (have > maxFrames) {
            long long drop = have - maxFrames;
            ring_.erase(ring_.begin(), ring_.begin() + (size_t)(drop * 2));
            pending_ -= drop;
        }
        pumpLocked();
        return true;
    }

    long long consumed() const override { return consumed_; }
    long long pending() const override { return pending_; }
    void resetClock() override {
        std::lock_guard<std::mutex> lk(mx_);
        consumed_ = 0;
    }

    void setVolume(int percent) override {
        if (percent < 0)
            percent = 0;
        if (percent > 100)
            percent = 100;
        if (!hw_)
            return;
        WORD v = (WORD)(percent * 0xFFFF / 100);
        waveOutSetVolume(hw_, MAKELONG(v, v));
    }

    bool isDevice() const override { return true; }

private:
    static constexpr int kChunkFrames = 4096; // ~85 ms
    static constexpr int kMaxChunks = 8;
    static constexpr int kPrime = 2;

    struct WaveHdr {
        WAVEHDR hdr{};
        std::vector<int16_t> data;
        int frames = 0;
        bool real = false;
    };

    static void CALLBACK callback(HWAVEOUT, UINT msg, DWORD_PTR inst, DWORD_PTR param1, DWORD_PTR) {
        auto *self = (WaveOutSink *)inst;
        if (!self || msg != WOM_DONE)
            return;
        auto *wh = (WaveHdr *)param1;
        self->onDone(wh);
    }

    void onDone(WaveHdr *wh) {
        std::lock_guard<std::mutex> lk(mx_);
        // Drop from pool bookkeeping.
        for (size_t i = 0; i < pool_.size(); ++i)
            if (pool_[i] == wh) {
                pool_.erase(pool_.begin() + i);
                break;
            }
        if (open_) {
            if (wh->real) {
                pending_ -= wh->frames;
                consumed_ += wh->frames;
            }
            --inFlight_;
        }
        delete wh;
        if (!open_)
            return;
        if (!ring_.empty())
            feedRealLocked();
        else
            primeZeroLocked();
    }

    // Move up to one chunk of ring data into a new in-flight header.
    void feedRealLocked() {
        int frames = kChunkFrames;
        long long have = ring_.size() / 2;
        if (frames > (int)have)
            frames = (int)have;
        if (frames <= 0)
            return;
        auto *wh = new WaveHdr;
        wh->frames = frames;
        wh->real = true;
        wh->data.assign(ring_.begin(), ring_.begin() + frames * 2);
        ring_.erase(ring_.begin(), ring_.begin() + frames * 2);
        // Keep the ring vector bounded.
        if (ring_.size() > 8LL * 1024 * 1024)
            ring_.erase(ring_.begin(), ring_.begin());
        wh->hdr.lpData = (LPSTR)wh->data.data();
        wh->hdr.dwBufferLength = (DWORD)(frames * 4);
        wh->hdr.dwFlags = 0;
        pool_.push_back(wh);
        MMRESULT rr = waveOutPrepareHeader(hw_, &wh->hdr, sizeof(WAVEHDR));
        if (rr != MMSYSERR_NOERROR) {
            pool_.pop_back();
            delete wh;
            ring_.insert(ring_.begin(), wh->data.begin(), wh->data.end());
            return;
        }
        rr = waveOutWrite(hw_, &wh->hdr, sizeof(WAVEHDR));
        if (rr != MMSYSERR_NOERROR) {
            waveOutUnprepareHeader(hw_, &wh->hdr, sizeof(WAVEHDR));
            pool_.pop_back();
            delete wh;
            ring_.insert(ring_.begin(), wh->data.begin(), wh->data.end());
            return;
        }
        ++inFlight_;
    }

    void primeZeroLocked() {
        if (inFlight_ >= kPrime * 2)
            return;
        auto *wh = new WaveHdr;
        wh->frames = kChunkFrames;
        wh->real = false;
        wh->data.assign((size_t)kChunkFrames * 2, 0);
        wh->hdr.lpData = (LPSTR)wh->data.data();
        wh->hdr.dwBufferLength = (DWORD)(kChunkFrames * 4);
        wh->hdr.dwFlags = 0;
        pool_.push_back(wh);
        MMRESULT rr = waveOutPrepareHeader(hw_, &wh->hdr, sizeof(WAVEHDR));
        if (rr != MMSYSERR_NOERROR) {
            pool_.pop_back();
            delete wh;
            return;
        }
        rr = waveOutWrite(hw_, &wh->hdr, sizeof(WAVEHDR));
        if (rr != MMSYSERR_NOERROR) {
            waveOutUnprepareHeader(hw_, &wh->hdr, sizeof(WAVEHDR));
            pool_.pop_back();
            delete wh;
            return;
        }
        ++inFlight_;
    }

    void pumpLocked() {
        while (open_ && inFlight_ < kPrime && !ring_.empty())
            feedRealLocked();
    }

    mutable std::mutex mx_;
    HWAVEOUT hw_ = nullptr;
    std::atomic<bool> open_{false};
    int rate_ = 48000;
    std::vector<int16_t> ring_;
    std::vector<WaveHdr *> pool_;
    std::atomic<long long> consumed_{0};
    std::atomic<long long> pending_{0};
    int inFlight_ = 0;
};
