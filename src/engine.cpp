// Playback Machine — playback engine implementation.
//
// Two threads:
//   * decodeLoop  : demux + decode video/audio, resample audio to 48 kHz
//                   s16 stereo, maintain the decoded-frame queues and the
//                   audio buffers, perform seeks, pump the audio sink.
//   * presentLoop : schedules frame display (audio-master or wall-clock),
//                   frame stepping, reverse playback, seek previews.
//
// All displayed frames are converted to RGBA by swscale (only the frames
// actually shown are converted).

#include "engine.h"
#include "audiosink.h"
#include "ffdyn.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace {


double nowSec() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}
void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
double clampD(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }
const char *hwTypeName(AVHWDeviceType t) {
    switch (t) {
        case AV_HWDEVICE_TYPE_VDPAU: return "vdpau";
        case AV_HWDEVICE_TYPE_CUDA: return "cuda";
        case AV_HWDEVICE_TYPE_VAAPI: return "vaapi";
        case AV_HWDEVICE_TYPE_DXVA2: return "dxva2";
        case AV_HWDEVICE_TYPE_D3D11VA: return "d3d11va";
        case AV_HWDEVICE_TYPE_VIDEOTOOLBOX: return "videotoolbox";
        default: return "hw";
    }
}
std::string avStrErr(int e) {
    char buf[128] = {0};
    av_strerror(e, buf, sizeof(buf));
    return std::string(buf);
}
inline int64_t estBytes(const AVFrame *f) {
    return f ? (int64_t)f->width * f->height * 3 / 2 : 0;
}

} // namespace

struct PlaybackEngine::Impl {
    // ------------------------------------------------------------------
    // demux / decode state (owned by the decode thread)
    // ------------------------------------------------------------------
    AVFormatContext *fmt = nullptr;
    AVStream *vst = nullptr, *ast = nullptr;
    AVCodecContext *vctx = nullptr, *actx = nullptr;
    SwrContext *swr = nullptr;
    AVBufferRef *hwDeviceRef = nullptr;
    bool hwOn = false;
    int vIdx = -1, aIdx = -1;
    double synthPts = -1;

    struct VFrame {
        AVFrame *av = nullptr;
        double pts = 0;
    };
    static constexpr int64_t kQueueCapBytes = 128LL * 1024 * 1024;
    static constexpr int kQueueCapFrames = 32; // ~1 s of lookahead (backpressured)
    static constexpr int64_t kHistCapBytes = 512LL * 1024 * 1024;
    static constexpr int kHistCapFrames = 400;
    // Set whenever the playhead moves to an earlier time; the forward queue
    // then holds (future) frames from the old position and must be rebuilt.
    std::atomic<bool> queueStale{false};

    std::mutex vqMtx;
    std::condition_variable vqCv;
    std::deque<VFrame> vq;
    int64_t vqBytes = 0;

    std::mutex histMtx;
    std::deque<VFrame> hist;
    int64_t histBytes = 0;

    // audio (48 kHz s16 interleaved; a "frame" = 2 samples)
    AudioSink *sink_ = nullptr;
    std::atomic<AudioSink *> sink{nullptr};
    std::mutex aMtx;
    std::vector<int16_t> aBuf;     // future audio; [aFront .. end) live
    int aFront = 0;
    double aNextPts = 0;           // pts of aBuf end (approx)
    std::vector<int16_t> aPlayed;  // recently played (tail = playhead), <= 400 ms
    std::vector<int16_t> revBuf;   // reversed audio for reverse playback
    std::atomic<bool> reverseAudio{false};
    int deviceRateReq = 0;         // under aMtx; 0 = mute
    std::atomic<long> audioReq{0};
    std::atomic<int> audioReqType{0}; // 1=renrate 2=forward-resync 3=reverse-start
    long lastAudioReq = 0;
    std::vector<uint8_t> convertBuf;

    // seek / generation
    std::atomic<uint64_t> gen{0};
    std::atomic<long> seekReq{0};
    std::atomic<long> lastSeekDone{0}; // last seek processed by the decode thread
    std::atomic<double> seekTargetV{0.0};
    std::mutex decodeCvMtx;
    std::condition_variable decodeCv;
    std::atomic<bool> eof{false};
    std::atomic<bool> stopped{false};
    std::atomic<bool> opened{false};

    std::atomic<double> speedV_{1.0};
    std::atomic<int> dirV_{1};
    std::atomic<bool> loopV_{false};
    std::atomic<PlayerState> stateV_{PlayerState::Stopped};

    // presentation
    std::mutex fMtx;
    std::vector<uint8_t> fBuf;
    int fW = 0, fH = 0;
    double fPts = 0;
    uint64_t fToken = 0;
    std::atomic<double> curPts{0.0};

    std::atomic<bool> playing{false};
    std::mutex stepMtx;
    std::deque<int> stepQueue;

    SwsContext *sws = nullptr;
    int swsW = 0, swsH = 0;
    AVPixelFormat swsIn = AV_PIX_FMT_NONE;
    AVFrame *sysFrame = nullptr;
    AVPixelFormat hwSysFmt = AV_PIX_FMT_NONE; // system format for the hw decoder output

    EngineInfo info;
    double staleGap() const {
        double g = info.fps > 0 ? 15.0 / info.fps : 0.5;
        return std::max(0.6, g); // safety net only; backward moves use queueStale
    }

    std::string errMsg;
    std::mutex errMtx;
    void setErr(const std::string &s) {
        std::lock_guard<std::mutex> lk(errMtx);
        errMsg = s;
    }

    std::thread decodeThread, presentThread;

    // ------------------------------------------------------------------
    // small helpers
    // ------------------------------------------------------------------
    void freeVFrame(VFrame *vf) {
        if (vf->av)
            av_frame_free(&vf->av);
        vf->av = nullptr;
    }
    void requestSeek(double t) {
        seekTargetV.store(t);
        seekReq.fetch_add(1);
        decodeCv.notify_all();
    }
    double duration() const { return info.duration; }

    // ------------------------------------------------------------------
    // audio
    // ------------------------------------------------------------------
    void appendAudio(int nFrames, const int16_t *samples, double pts) {
        if (nFrames <= 0)
            return;
        bool doWrite = false;
        {
            std::lock_guard<std::mutex> lk(aMtx);
            int live = (int)aBuf.size() / 2 - aFront;
            if (live <= 0)
                aNextPts = pts; // (re)anchor after a seek/trim
            aBuf.insert(aBuf.end(), samples, samples + nFrames * 2);
            aNextPts += nFrames / 48000.0;
            const int64_t kMaxAudioFrames = 96000; // 2 s
            int64_t excess = (int64_t)aBuf.size() / 2 - aFront - kMaxAudioFrames;
            if (excess > 0) {
                aFront += (int)excess;
                aNextPts -= excess / 48000.0;
            }
            if (aFront > 32768) {
                aBuf.erase(aBuf.begin(), aBuf.begin() + (size_t)aFront * 2);
                aFront = 0;
            }
            doWrite = !reverseAudio.load();
        }
        AudioSink *sk = sink.load();
        if (doWrite && sk && sk->isOpen())
            sk->write(samples, nFrames);
    }

    void onAudioFrame(const AVFrame *f, bool flush) {
        if (!swr)
            return;
        double atb = av_q2d(ast->time_base);
        double pts = f->pts != AV_NOPTS_VALUE ? f->pts * atb : aNextPts;
        int capFrames = f->nb_samples + 2048;
        convertBuf.resize((size_t)capFrames * 4);
        uint8_t *out[1] = { convertBuf.data() };
        // For planar input formats (e.g. FLTP) `in_buf` must be the array of
        // plane pointers, i.e. f->data itself.
        int got = swr_convert(swr, out, capFrames,
                              (const uint8_t **)f->data, f->nb_samples);
        if (flush) {
            int got2 = swr_convert(swr, out, capFrames, nullptr, 0);
            if (got2 > got) {
                got = got2; // flush tail
            }
        }
        if (got > 0)
            appendAudio(got / 2, (const int16_t *)out[0], pts);
    }

    void rebuildRevBufLocked() {
        int playedF = (int)aPlayed.size() / 2;
        int bufF = (int)aBuf.size() / 2 - aFront;
        const int kWant = 19200; // 400 ms
        int need = kWant;
        int takeB = std::min(need, bufF);
        need -= takeB;
        int takeP = std::min(need, playedF);
        std::vector<int16_t> src((size_t)(takeB + takeP) * 2, 0);
        if (takeB > 0)
            std::copy(&aBuf[(size_t)(aFront + bufF - takeB) * 2],
                      &aBuf[(size_t)(aFront + bufF) * 2], src.begin());
        if (takeP > 0)
            std::copy(&aPlayed[(size_t)(playedF - takeP) * 2],
                      &aPlayed[(size_t)playedF * 2], src.begin() + takeB * 2);
        size_t n = src.size() / 2;
        revBuf.resize(src.size());
        for (size_t i = 0; i < n; ++i) {
            size_t si = n - 1 - i;
            revBuf[i * 2] = src[si * 2];
            revBuf[i * 2 + 1] = src[si * 2 + 1];
        }
    }

    // Media pts of "now" per the audio device (invalid: < -1e6).
    double audioClock() {
        AudioSink *sk = sink.load();
        if (!sk || !sk->isOpen())
            return -1e9;
        long long c = sk->consumed();
        long long p = sk->pending();
        std::lock_guard<std::mutex> lk(aMtx);
        int totalF = (int)aBuf.size() / 2;
        if (totalF <= aFront)
            return -1e9;
        double basePts = aNextPts - (totalF - aFront) / 48000.0;
        return basePts + (c + p / 2.0) / 48000.0;
    }

    void audioPump() {
        AudioSink *sk = sink.load();
        long r = audioReq.load();
        if (r != lastAudioReq) {
            lastAudioReq = r;
            int t = audioReqType.load();
            int rate = 0;
            {
                std::lock_guard<std::mutex> lk(aMtx);
                rate = deviceRateReq;
            }
            if (t == 1) { // re-rate (speed change)
                if (sk) {
                    if (sk->isOpen())
                        sk->close();
                    if (rate > 0) {
                        if (sk->open(rate))
                            sk->resetClock();
                    }
                }
            } else if (t == 2) { // forward resync (came back from reverse)
                if (sk && sk->isOpen())
                    sk->close();
                {
                    std::lock_guard<std::mutex> lk(aMtx);
                    double cur = curPts.load();
                    int totalF = (int)aBuf.size() / 2;
                    if (totalF > aFront) {
                        double basePts = aNextPts - (totalF - aFront) / 48000.0;
                        int64_t skip = (int64_t)((cur - basePts + 0.1) * 48000.0);
                        if (skip < 0)
                            skip = 0;
                        if (skip > totalF - aFront)
                            skip = totalF - aFront;
                        aFront += (int)skip;
                    }
                    aPlayed.clear();
                    revBuf.clear();
                }
                if (sk && rate > 0) {
                    if (sk->open(rate))
                        sk->resetClock();
                }
            } else if (t == 3) { // reverse start
                if (sk) {
                    if (sk->isOpen())
                        sk->close();
                    if (rate > 0) {
                        if (sk->open(rate))
                            sk->resetClock();
                    }
                }
                std::lock_guard<std::mutex> lk(aMtx);
                rebuildRevBufLocked();
            }
        }
        if (!sk || !sk->isOpen())
            return;
        if (reverseAudio.load()) {
            long long c = sk->consumed();
            std::lock_guard<std::mutex> lk(aMtx);
            int totalF = (int)revBuf.size() / 2;
            int remaining = totalF - (int)c;
            if (remaining <= 0) {
                rebuildRevBufLocked();
                sk->resetClock();
                c = 0;
                totalF = (int)revBuf.size() / 2;
                remaining = totalF;
            }
            if (totalF > 0 && remaining > 0 && (long long)sk->pending() < 4096) {
                int toFeed = remaining < 4096 ? remaining : 4096;
                sk->write(&revBuf[(size_t)c * 2], toFeed);
            }
        } else {
            long long c = sk->consumed();
            long long p = sk->pending();
            std::lock_guard<std::mutex> lk(aMtx);
            int totalF = (int)aBuf.size() / 2;
            if (totalF > aFront) {
                double basePts = aNextPts - (totalF - aFront) / 48000.0;
                double clock = basePts + (c + p / 2.0) / 48000.0;
                double moveUpTo = clock - 0.02;
                if (moveUpTo > basePts) {
                    int64_t nMove = (int64_t)((moveUpTo - basePts) * 48000.0);
                    if (nMove > totalF - aFront)
                        nMove = totalF - aFront;
                    if (nMove > 0) {
                        aPlayed.insert(aPlayed.end(), aBuf.begin() + (size_t)aFront * 2,
                                        aBuf.begin() + (size_t)(aFront + nMove) * 2);
                        aFront += (int)nMove;
                    }
                }
            }
            const size_t kPlayedCap = 19200 * 2; // 400 ms
            if (aPlayed.size() > kPlayedCap)
                aPlayed.erase(aPlayed.begin(), aPlayed.begin() + (long long)(aPlayed.size() - kPlayedCap));
            if (aFront > 32768) {
                aBuf.erase(aBuf.begin(), aBuf.begin() + (size_t)aFront * 2);
                aFront = 0;
            }
        }
    }

    // ------------------------------------------------------------------
    // video frames
    // ------------------------------------------------------------------
    void onVideoFrame(const AVFrame *f) {
        double tb = vst ? av_q2d(vst->time_base) : 1.0 / 30.0;
        double step = info.fps > 0 ? 1.0 / info.fps : 1.0 / 30.0;
        double pts;
        if (f->pts != AV_NOPTS_VALUE) {
            pts = f->pts * tb;
            synthPts = pts;
        } else {
            pts = (synthPts < 0 ? 0.0 : synthPts) + step;
            synthPts = pts;
        }
        int bytes = (int)estBytes(f);

        VFrame vf;
        vf.av = av_frame_alloc();
        if (!vf.av || av_frame_ref(vf.av, f) < 0) {
            if (vf.av)
                av_frame_free(&vf.av);
            return;
        }
        vf.pts = pts;

        {
            std::unique_lock<std::mutex> lk(vqMtx);
            if (dirV_.load() < 0) {
                // Reverse: the forward queue is not consumed, so it must not
                // backpressure the decoder. Let the decoder decode up to the
                // playhead (building the history the reverse pass plays
                // back), then block. Skip the queue push; it is rebuilt when
                // we return to forward.
                if (pts >= curPts.load() + 0.05) {
                    int tries = 0;
                    while (pts >= curPts.load() + 0.05 && !stopped.load()) {
                        if (seekReq.load() != lastSeekDone.load())
                            return; // a seek (refill / forward) supersedes this
                        vqCv.wait_for(lk, std::chrono::milliseconds(20));
                        if (++tries > 500)
                            break; // ~10 s safety: let it through
                    }
                }
            } else {
                // Forward: block until presentation makes room. Never drop
                // the front (the presentation reads from there).
                int tries = 0;
                while ((vq.size() >= (size_t)kQueueCapFrames ||
                        (vqBytes + bytes > kQueueCapBytes && !vq.empty()))) {
                    if (stopped.load())
                        return;
                    if (seekReq.load() != lastSeekDone.load())
                        return; // a seek is pending; this frame would be discarded
                    vqCv.wait_for(lk, std::chrono::milliseconds(20));
                    if (++tries > 100) {
                        // Presentation has stalled for ~2 s; drop the oldest
                        // to keep decoding (last resort, avoids a deadlock).
                        vqBytes -= estBytes(vq.front().av);
                        freeVFrame(&vq.front());
                        vq.pop_front();
                        tries = 0;
                    }
                }
                vq.push_back(vf);
                vqBytes += bytes;
            }
        }
        vqCv.notify_all();
        {
            VFrame vh;
            vh.av = av_frame_alloc();
            if (vh.av && av_frame_ref(vh.av, f) == 0) {
                vh.pts = pts;
                std::lock_guard<std::mutex> lk(histMtx);
                while ((hist.size() >= (size_t)kHistCapFrames ||
                        (histBytes + bytes > kHistCapBytes && !hist.empty()))) {
                    histBytes -= estBytes(hist.front().av);
                    freeVFrame(&hist.front());
                    hist.pop_front();
                }
                hist.push_back(std::move(vh));
                histBytes += bytes;
            } else {
                if (vh.av)
                    av_frame_free(&vh.av);
            }
        }
        vqCv.notify_all();
    }

    // Newest (largest-pts) frame currently in history; -1e9 if empty.
    double histNewestPts() {
        std::lock_guard<std::mutex> lk(histMtx);
        return hist.empty() ? -1e9 : hist.back().pts;
    }

    // Newest frame in history strictly older than cur.
    bool tryPopHistory(double cur, VFrame *out) {
        std::lock_guard<std::mutex> lk(histMtx);
        for (auto it = hist.rbegin(); it != hist.rend(); ++it) {
            if (it->pts < cur - 0.001) {
                histBytes -= estBytes(it->av);
                *out = std::move(*it);
                hist.erase(std::next(it).base());
                return true;
            }
        }
        return false;
    }

    // ------------------------------------------------------------------
    // seek
    // ------------------------------------------------------------------
    void performSeek(double t) {
        double d = info.duration;
        if (d > 0)
            t = clampD(t, 0.0, d - 0.02);
        else if (t < 0)
            t = 0;
        {
            std::lock_guard<std::mutex> lk(vqMtx);
            while (!vq.empty()) {
                vqBytes -= estBytes(vq.front().av);
                freeVFrame(&vq.front());
                vq.pop_front();
            }
            vqBytes = 0;
        }
        {
            std::lock_guard<std::mutex> lk(histMtx);
            while (!hist.empty()) {
                histBytes -= estBytes(hist.front().av);
                freeVFrame(&hist.front());
                hist.pop_front();
            }
            histBytes = 0;
        }
        int64_t ts = (int64_t)(t * AV_TIME_BASE);
        if (vIdx >= 0)
            av_seek_frame(fmt, vIdx, av_rescale_q(ts, AV_TIME_BASE_Q, vst->time_base), AVSEEK_FLAG_BACKWARD);
        if (aIdx >= 0)
            av_seek_frame(fmt, aIdx, av_rescale_q(ts, AV_TIME_BASE_Q, ast->time_base), AVSEEK_FLAG_BACKWARD);
        if (vctx)
            avcodec_flush_buffers(vctx);
        if (actx)
            avcodec_flush_buffers(actx);
        if (swr) {
            swr_close(swr);
            swr_init(swr);
        }
        {
            std::lock_guard<std::mutex> lk(aMtx);
            aBuf.clear();
            aFront = 0;
            aPlayed.clear();
            revBuf.clear();
            aNextPts = t;
        }
        AudioSink *sk = sink.load();
        if (sk)
            sk->resetClock();
        eof.store(false);
        synthPts = -1;
        queueStale.store(false); // the queue is rebuilt from the seek point
        vqCv.notify_all();
    }

    // ------------------------------------------------------------------
    // render (presentation thread only)
    // ------------------------------------------------------------------
    void renderFrame(const VFrame &vf) {
        const AVFrame *src = vf.av;
        if (!src)
            return;
        if (vctx && vctx->hw_frames_ctx && src->hw_frames_ctx) {
            if (hwSysFmt == AV_PIX_FMT_NONE) {
                enum AVPixelFormat *fmts = nullptr;
                hwSysFmt = AV_PIX_FMT_NV12; // sane default
                if (av_hwframe_transfer_get_formats(src->hw_frames_ctx,
                                                    AV_HWFRAME_TRANSFER_DIRECTION_FROM, &fmts, 0) >= 0 &&
                    fmts) {
                    hwSysFmt = fmts[0];
                    av_free(fmts);
                }
            }
            if (!sysFrame)
                sysFrame = av_frame_alloc();
            if (sysFrame &&
                (sysFrame->width != src->width || sysFrame->height != src->height ||
                 sysFrame->format != (int)hwSysFmt || !sysFrame->data[0])) {
                av_frame_unref(sysFrame);
                sysFrame->width = src->width;
                sysFrame->height = src->height;
                sysFrame->format = (int)hwSysFmt;
                if (av_frame_get_buffer(sysFrame, 32) < 0)
                    return;
            }
            if (av_hwframe_transfer_data(sysFrame, src, 0) < 0)
                return;
            src = sysFrame;
        }
        int ow = src->width, oh = src->height;
        if (ow <= 0 || oh <= 0)
            return;
        if (!sws || swsW != ow || swsH != oh || swsIn != (int)src->format) {
            if (sws)
                sws_freeContext(&sws);
            sws = sws_getContext(ow, oh, (enum AVPixelFormat)src->format, ow, oh, AV_PIX_FMT_RGBA,
                                 SWS_BILINEAR, nullptr, nullptr, nullptr);
            if (!sws)
                return;
            swsW = ow;
            swsH = oh;
            swsIn = (AVPixelFormat)src->format;
        }
        std::vector<uint8_t> tmp((size_t)ow * oh * 4);
        uint8_t *dst[1] = { tmp.data() };
        int ds[1] = { ow * 4 };
        if (sws_scale(sws, src->data, src->linesize, 0, oh, dst, ds) < 0)
            return;
        {
            std::lock_guard<std::mutex> lk(fMtx);
            fBuf.swap(tmp);
            fW = ow;
            fH = oh;
            fPts = vf.pts;
            fToken++;
        }
    }

    // ------------------------------------------------------------------
    // decode thread
    // ------------------------------------------------------------------
    void drainDecoders(AVFrame *vout, AVFrame *aout) {
        if (vctx) {
            avcodec_send_packet(vctx, nullptr);
            for (;;) {
                av_frame_unref(vout);
                int r = avcodec_receive_frame(vctx, vout);
                if (r != 0)
                    break;
                onVideoFrame(vout);
            }
        }
        if (actx) {
            avcodec_send_packet(actx, nullptr);
            for (;;) {
                av_frame_unref(aout);
                int r = avcodec_receive_frame(actx, aout);
                if (r != 0)
                    break;
                onAudioFrame(aout, true);
            }
            if (swr) {
                int capF = 2048;
                convertBuf.resize((size_t)capF * 4);
                uint8_t *out[1] = { convertBuf.data() };
                int got = swr_convert(swr, out, capF, nullptr, 0);
                if (got > 0)
                    appendAudio(got / 2, (const int16_t *)out[0], aNextPts);
            }
        }
    }

    void decodeLoop() {
        AVPacket *pkt = av_packet_alloc();
        AVFrame *vout = av_frame_alloc();
        AVFrame *aout = av_frame_alloc();
        long lastSeek = 0;
        while (!stopped.load()) {
            long r = seekReq.load();
            if (r != lastSeek) {
                performSeek(seekTargetV.load());
                lastSeek = r;
                lastSeekDone.store(r);
                gen.fetch_add(1);
            }
            audioPump();
            if (eof.load()) {
                // At EOF av_read_frame returns instantly, so tick (bounded
                // wait) to keep servicing seeks / stop.
                {
                    std::unique_lock<std::mutex> lk(decodeCvMtx);
                    decodeCv.wait_for(lk, std::chrono::milliseconds(25),
                                      [this, lastSeek] {
                                          return stopped.load() ||
                                                 seekReq.load() != lastSeek;
                                      });
                }
                continue;
            }
            // Normal case: av_read_frame blocks until the next packet is
            // available, so the decoder runs ahead of presentation.
            int ret = av_read_frame(fmt, pkt);
            if (ret < 0) {
                if (ret == AVERROR_EOF) {
                    if (!eof.load()) {
                        drainDecoders(vout, aout);
                        eof.store(true);
                    }
                    av_packet_unref(pkt);
                    continue;
                }
                setErr("Read error: " + avStrErr(ret));
                playing.store(false);
                stateV_.store(PlayerState::Error);
                break;
            }
            int r2;
            if (pkt->stream_index == vIdx) {
                r2 = avcodec_send_packet(vctx, pkt);
                if (r2 == 0 || r2 == AVERROR(EAGAIN)) {
                    for (;;) {
                        av_frame_unref(vout);
                        r2 = avcodec_receive_frame(vctx, vout);
                        if (r2 == AVERROR(EAGAIN) || r2 == AVERROR_EOF)
                            break;
                        if (r2 < 0)
                            break;
                        onVideoFrame(vout);
                    }
                }
            } else if (pkt->stream_index == aIdx) {
                r2 = avcodec_send_packet(actx, pkt);
                if (r2 == 0 || r2 == AVERROR(EAGAIN)) {
                    for (;;) {
                        av_frame_unref(aout);
                        r2 = avcodec_receive_frame(actx, aout);
                        if (r2 == AVERROR(EAGAIN) || r2 == AVERROR_EOF)
                            break;
                        if (r2 < 0)
                            break;
                        onAudioFrame(aout, false);
                    }
                }
            }
            av_packet_unref(pkt);
        }
        av_frame_free(&vout);
        av_frame_free(&aout);
        av_packet_free(&pkt);
    }

    // ------------------------------------------------------------------
    // presentation thread
    // ------------------------------------------------------------------
    void presentForward(double s, double &wall0, double &media0, double lastSeekTarget) {
        VFrame vf;
        bool got = false;
        {
        std::unique_lock<std::mutex> lk(vqMtx);
        for (;;) {
            if (stopped.load())
                return;
            // A backward move (step/seek/reverse) left future frames from
            // the old position in the queue: rebuild it from the playhead.
            // Wait for the seek to land so the next call sees the fresh
            // seek target (else its discard loop over-drops the re-decoded
            // frames and manufactures a discontinuity).
            if (queueStale.exchange(false)) {
                double t = curPts.load();
                uint64_t genBefore = gen.load();
                lk.unlock();
                requestSeek(t);
                for (int i = 0; i < 150 && !stopped.load(); ++i) {
                    if (gen.load() != genBefore)
                        break;
                    sleepMs(10);
                }
                return;
            }
            while (vq.empty()) {
                if (stopped.load())
                    return;
                if (eof.load()) {
                    lk.unlock();
                    if (loopV_.load()) {
                        requestSeek(0.0);
                        return;
                    }
                    playing.store(false);
                    stateV_.store(PlayerState::Ended);
                    return;
                }
                vqCv.wait_for(lk, std::chrono::milliseconds(30));
            }
                // Compare against the later of the playhead and the current
                // playback epoch's reference (a forward seek moves media0
                // ahead of the not-yet-refreshed playhead).
                double cur = curPts.load();
                double ref = media0 > cur ? media0 : cur;
                double gap = vq.front().pts - ref;
                if (gap > staleGap()) {
                    // Safety net: queue jumped far ahead of the playhead
                    // (a discontinuity). Rebuild it from the playhead, then
                    // wait for the seek to land so we don't re-fire on the
                    // still-stale queue (which caused a re-seek storm).
                    uint64_t genBefore = gen.load();
                    lk.unlock();
                    requestSeek(cur);
                    for (int i = 0; i < 150 && !stopped.load(); ++i) {
                        if (gen.load() != genBefore)
                            break;
                        sleepMs(10);
                    }
                    return;
                }
                while (!vq.empty() && vq.front().pts < lastSeekTarget - 0.001) {
                    vqBytes -= estBytes(vq.front().av);
                    freeVFrame(&vq.front());
                    vq.pop_front();
                }
                if (!vq.empty()) {
                    vf = std::move(vq.front());
                    vq.pop_front();
                    vqBytes -= estBytes(vf.av);
                    got = true;
                    break;
                }
            }
        }
        if (got)
            vqCv.notify_all(); // free queue space for the (blocked) decoder
        if (!got)
            return;
        bool audioMaster = false;
        if (info.hasAudio) {
            AudioSink *sk = sink.load();
            if (sk && sk->isOpen() && sk->isDevice())
                audioMaster = true;
        }
        if (audioMaster) {
            for (int i = 0; i < 400; ++i) {
                if (stopped.load())
                    return;
                double ac = audioClock();
                if (ac < -1e6)
                    break;
                if (ac >= vf.pts - 0.045)
                    break;
                if (vf.pts - ac > 0.9)
                    break;
                sleepMs(5);
            }
        } else {
            double deadline = wall0 + (vf.pts - media0) / s;
            double now = nowSec();
            while (now < deadline) {
                if (stopped.load())
                    return;
                sleepMs(5);
                now = nowSec();
            }
            if (nowSec() > deadline + 0.2) {
                wall0 = nowSec();
                media0 = vf.pts;
            }
        }
        renderFrame(vf);
        curPts.store(vf.pts);
    }

    void presentReverse(double s, double &wall0, double &media0, uint64_t &lastGen) {
        VFrame vf;
        double cur = curPts.load();
        if (!tryPopHistory(cur, &vf)) {
            double t = cur - 5.0;
            if (t < 0)
                t = 0;
            if (cur - t >= 0.3)
                requestSeek(t);
            bool ok = false;
            // Wait until the decoder has decoded up to near the playhead
            // (in reverse it decodes freely up to the playhead), then take
            // the frame just before it: seamless continuation.
            for (int i = 0; i < 150 && !stopped.load(); ++i) {
                sleepMs(20);
                if (gen.load() != lastGen) {
                    lastGen = gen.load();
                    wall0 = nowSec();
                    media0 = cur;
                }
                if (histNewestPts() >= cur - 0.05 && tryPopHistory(cur, &vf)) {
                    ok = true;
                    break;
                }
            }
            if (!ok)
                ok = tryPopHistory(cur, &vf); // fallback: whatever is there
            if (!ok) {
                if (cur <= 0.1 && loopV_.load()) {
                    dirV_.store(1); // auto-flip to forward at the very start
                    queueStale.store(true);
                    return;
                }
                sleepMs(60);
                return;
            }
        }
        // A fallback refill may hand us a frame well behind the playhead;
        // skip the unplayable gap (rebase) instead of freezing on it.
        if (media0 - vf.pts > 0.2) {
            wall0 = nowSec();
            media0 = vf.pts;
        }
        double deadline = wall0 + (media0 - vf.pts) / s;
        double now = nowSec();
        while (now < deadline) {
            if (stopped.load())
                return;
            sleepMs(5);
            now = nowSec();
        }
        if (nowSec() > deadline + 0.2) {
            wall0 = nowSec();
            media0 = vf.pts;
        }
        renderFrame(vf);
        curPts.store(vf.pts);
    }

    void stepFrameForward() {
        double cur = curPts.load();
        VFrame vf;
        bool got = false;
        {
            std::unique_lock<std::mutex> lk(vqMtx);
            for (int i = 0; i < 15 && !got && !stopped.load(); ++i) {
                while (!vq.empty() && vq.front().pts <= cur + 0.001) {
                    vqBytes -= estBytes(vq.front().av);
                    freeVFrame(&vq.front());
                    vq.pop_front();
                }
                if (!vq.empty()) {
                    if (vq.front().pts - cur > staleGap()) {
                        lk.unlock();
                        requestSeek(cur);
                        lk.lock();
                        cur = curPts.load();
                        continue;
                    }
                    vf = std::move(vq.front());
                    vq.pop_front();
                    vqBytes -= estBytes(vf.av);
                    got = true;
                    break;
                }
                if (eof.load())
                    break;
                vqCv.wait_for(lk, std::chrono::milliseconds(100));
            }
        }
        if (got) {
            renderFrame(vf);
            curPts.store(vf.pts);
        }
    }

    void stepFrameBackward() {
        double cur = curPts.load();
        VFrame vf;
        if (!tryPopHistory(cur, &vf)) {
            double t = cur - 3.0;
            if (t < 0)
                t = 0;
            if (cur - t >= 0.3)
                requestSeek(t);
            for (int i = 0; i < 125 && !stopped.load(); ++i) {
                sleepMs(20);
                if (tryPopHistory(cur, &vf))
                    break;
                if (cur <= 0.1)
                    break;
            }
        }
        if (vf.av) {
            renderFrame(vf);
            curPts.store(vf.pts);
            queueStale.store(true); // forward queue no longer starts here
        }
    }

    void doPreview(double target) {
        VFrame vf;
        bool got = false;
        {
            std::unique_lock<std::mutex> lk(vqMtx);
            for (int i = 0; i < 40 && !got && !stopped.load(); ++i) {
                while (!vq.empty() && vq.front().pts < target - 0.001) {
                    vqBytes -= estBytes(vq.front().av);
                    freeVFrame(&vq.front());
                    vq.pop_front();
                }
                if (!vq.empty()) {
                    vf = std::move(vq.front());
                    vq.pop_front();
                    vqBytes -= estBytes(vf.av);
                    got = true;
                    break;
                }
                if (eof.load())
                    break;
                vqCv.wait_for(lk, std::chrono::milliseconds(100));
            }
        }
        if (got) {
            renderFrame(vf);
            curPts.store(vf.pts);
        } else if (eof.load()) {
            stateV_.store(PlayerState::Ended);
        }
    }

    void presentLoop() {
        double lastSpeed = -1, lastDir = -1;
        uint64_t lastGen = 0;
        double wall0 = 0, media0 = 0;
        double lastSeekTarget = 0;
        while (!stopped.load()) {
            if (!opened.load()) {
                sleepMs(20);
                continue;
            }
            uint64_t g = gen.load();
            bool genChanged = false;
            if (g != lastGen) {
                lastGen = g;
                lastSeekTarget = seekTargetV.load();
                genChanged = true;
                if (dirV_.load() > 0) {
                    wall0 = nowSec();
                    media0 = lastSeekTarget;
                }
                // reverse refill seeks: reference is handled in presentReverse
            }
            double s = speedV_.load();
            int d = dirV_.load();
            if (s != lastSpeed || d != lastDir) {
                wall0 = nowSec();
                media0 = curPts.load();
                lastSpeed = s;
                lastDir = d;
            }
            if (playing.load()) {
                if (d > 0)
                    presentForward(s, wall0, media0, lastSeekTarget);
                else
                    presentReverse(s, wall0, media0, lastGen);
            } else {
                int step = 0;
                {
                    std::lock_guard<std::mutex> lk(stepMtx);
                    if (!stepQueue.empty()) {
                        step = stepQueue.front();
                        stepQueue.pop_front();
                    }
                }
                if (step != 0) {
                    if (step > 0)
                        stepFrameForward();
                    else
                        stepFrameBackward();
                } else if (genChanged) {
                    // A seek completed while paused: show its frame.
                    doPreview(lastSeekTarget);
                } else {
                    sleepMs(15);
                }
            }
        }
    }

    // ------------------------------------------------------------------
    // hw accel probe
    // ------------------------------------------------------------------
    bool tryHwAccel(const AVCodec *codec) {
        // Collect the codec's HW_DEVICE_CTX configurations, then try them in
        // platform-preferred order (create a device of each type; the first
        // one the decoder accepts wins).
        struct Cfg {
            AVHWDeviceType type;
        };
        Cfg cfgs[8];
        int n = 0;
        for (int i = 0; n < 8; ++i) {
            const AVCodecHWConfig *c = avcodec_get_hw_config(codec, i);
            if (!c)
                break;
            if (c->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)
                cfgs[n++] = {c->device_type};
        }
        auto order = [](AVHWDeviceType t) -> int {
#if defined(_WIN32)
            switch (t) {
            case AV_HWDEVICE_TYPE_D3D11VA: return 0;
            case AV_HWDEVICE_TYPE_DXVA2: return 1;
            case AV_HWDEVICE_TYPE_CUDA: return 2;
            default: return 9;
            }
#else
            switch (t) {
            case AV_HWDEVICE_TYPE_CUDA: return 0;
            case AV_HWDEVICE_TYPE_VAAPI: return 1;
            case AV_HWDEVICE_TYPE_VDPAU: return 2;
            default: return 9;
            }
#endif
        };
        for (int a = 0; a < n; ++a)
            for (int b = a + 1; b < n; ++b)
                if (order(cfgs[b].type) < order(cfgs[a].type)) {
                    Cfg tmp = cfgs[a];
                    cfgs[a] = cfgs[b];
                    cfgs[b] = tmp;
                }
        for (int i = 0; i < n; ++i) {
            AVBufferRef *dev = nullptr;
            if (av_hwdevice_ctx_create(&dev, cfgs[i].type, nullptr, nullptr, 0) < 0)
                continue;
            vctx->hw_device_ctx = av_buffer_ref(dev);
            int r = avcodec_open2(vctx, codec, nullptr);
            if (r < 0) {
                av_buffer_unref(&vctx->hw_device_ctx);
                vctx->hw_device_ctx = nullptr;
                av_buffer_unref(&dev);
                continue;
            }
            hwOn = true;
            hwDeviceRef = dev;
            info.hwAccel = true;
            info.hwName = hwTypeName(cfgs[i].type);
            return true;
        }
        return false;
    }
};

// ======================================================================
// Public API
// ======================================================================

PlaybackEngine::PlaybackEngine() : p_(new Impl()) {}

PlaybackEngine::~PlaybackEngine() {
    close();
    delete p_;
    p_ = nullptr;
}

bool PlaybackEngine::ffLoad(std::string *errOut) { return ff_dyn_load(errOut); }

bool PlaybackEngine::isReverse() const { return p_ && p_->dirV_.load() < 0; }
PlayerState PlaybackEngine::state() const { return p_ ? p_->stateV_.load() : PlayerState::Stopped; }
double PlaybackEngine::speed() const { return p_ ? p_->speedV_.load() : 1.0; }
int PlaybackEngine::direction() const { return p_ ? p_->dirV_.load() : 1; }
bool PlaybackEngine::isLoop() const { return p_ && p_->loopV_.load(); }
const EngineInfo &PlaybackEngine::info() const { return p_->info; }

bool PlaybackEngine::open(const std::string &path, std::string *errOut) {
    close();
    auto *im = p_;
    if (!ff_dyn_loaded() && !ff_dyn_load(errOut))
        return false;
    im->stopped.store(false);
    im->eof.store(false);
    im->gen.store(0);
    im->seekReq.store(0);
    im->lastSeekDone.store(0);
    im->seekTargetV.store(0.0);
    im->curPts.store(0.0);
    im->playing.store(false);
    im->queueStale.store(false);
    im->synthPts = -1;
    im->info = EngineInfo();

    if (avformat_open_input(&im->fmt, path.c_str(), nullptr, nullptr) < 0) {
        im->setErr("Cannot open '" + path + "' (missing file or unsupported format).");
        return false;
    }
    if (avformat_find_stream_info(im->fmt, nullptr) < 0) {
        im->setErr("Could not analyze '" + path + "'.");
        avformat_close_input(&im->fmt);
        return false;
    }
    const AVCodec *vcodec = nullptr, *acodec = nullptr;
    im->vIdx = av_find_best_stream(im->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &vcodec, 0);
    im->aIdx = av_find_best_stream(im->fmt, AVMEDIA_TYPE_AUDIO, -1, -1, &acodec, 0);
    if (im->vIdx < 0 && im->aIdx < 0) {
        im->setErr("No playable video or audio stream in '" + path + "'.");
        avformat_close_input(&im->fmt);
        return false;
    }
    if (im->vIdx >= 0) {
        im->vst = im->fmt->streams[im->vIdx];
        im->vctx = avcodec_alloc_context3(vcodec);
        bool ok = im->vctx && avcodec_parameters_to_context(im->vctx, im->vst->codecpar) >= 0;
        if (ok) {
            // Try hardware decoding first; fall back to software below.
            ok = im->tryHwAccel(vcodec);
            if (!ok)
                ok = avcodec_open2(im->vctx, vcodec, nullptr) >= 0;
        }
        if (!ok) {
            im->setErr("Cannot initialize video decoder (" + std::string(vcodec ? vcodec->name : "?") + ").");
            if (im->vctx)
                avcodec_free_context(&im->vctx);
            avformat_close_input(&im->fmt);
            return false;
        }
        im->info.hasVideo = true;
        im->info.width = im->vst->codecpar->width;
        im->info.height = im->vst->codecpar->height;
        im->info.videoCodec = vcodec ? vcodec->name : "?";
        im->info.fps = im->vst->avg_frame_rate.num > 0 ? av_q2d(im->vst->avg_frame_rate) : 0.0;
    }
    if (im->aIdx >= 0) {
        im->ast = im->fmt->streams[im->aIdx];
        im->actx = avcodec_alloc_context3(acodec);
        if (!im->actx || avcodec_parameters_to_context(im->actx, im->ast->codecpar) < 0 ||
            avcodec_open2(im->actx, acodec, nullptr) < 0) {
            im->setErr("Cannot initialize audio decoder (" + std::string(acodec ? acodec->name : "?") + ").");
            if (im->actx)
                avcodec_free_context(&im->actx);
            avformat_close_input(&im->fmt);
            return false;
        }
        im->info.hasAudio = true;
        im->info.audioCodec = acodec ? acodec->name : "?";
        // Resampler: anything in -> 48 kHz s16 stereo interleaved out.
        const AVChannelLayout *inLay = &im->actx->ch_layout;
        AVChannelLayout monoLayout = AV_CHANNEL_LAYOUT_MONO;
        AVChannelLayout stereoLayout = AV_CHANNEL_LAYOUT_STEREO;
        if (inLay->order == AV_CHANNEL_ORDER_UNSPEC)
            inLay = (im->actx->ch_layout.nb_channels == 1) ? &monoLayout : &stereoLayout;
        const AVChannelLayout outLay = AV_CHANNEL_LAYOUT_STEREO;
        im->swr = swr_alloc();
        if (im->swr &&
            swr_alloc_set_opts2(&im->swr, &outLay, AV_SAMPLE_FMT_S16, 48000,
                                inLay, im->actx->sample_fmt, im->actx->sample_rate, 0, nullptr) >= 0 &&
            swr_init(im->swr) >= 0) {
            // ready
        } else {
            if (im->swr)
                swr_free(&im->swr);
            im->info.hasAudio = false; // no audio if the resampler failed
        }
    }
    if (im->fmt->duration > 0)
        im->info.duration = im->fmt->duration / 1e6;
    else if (im->vIdx >= 0 && im->vst->duration > 0)
        im->info.duration = (double)im->vst->duration * av_q2d(im->vst->time_base);
    im->info.container = im->fmt->iformat ? im->fmt->iformat->name : "?";

    im->speedV_.store(1.0);
    im->dirV_.store(1);
    im->loopV_.store(false);
    {
        std::lock_guard<std::mutex> lk(im->aMtx);
        im->deviceRateReq = im->info.hasAudio ? 48000 : 0;
    }
    im->audioReqType.store(1);
    im->audioReq.fetch_add(1);
    im->playing.store(true);
    im->stateV_.store(PlayerState::Playing);
    im->opened.store(true);
    im->decodeThread = std::thread(&PlaybackEngine::Impl::decodeLoop, im);
    im->presentThread = std::thread(&PlaybackEngine::Impl::presentLoop, im);
    return true;
}

void PlaybackEngine::close() {
    auto *im = p_;
    if (!im)
        return;
    if (!im->opened.load() && !im->decodeThread.joinable()) {
        // nothing was ever opened
        return;
    }
    im->opened.store(false);
    im->playing.store(false);
    im->stopped.store(true);
    im->decodeCv.notify_all();
    im->vqCv.notify_all();
    if (im->decodeThread.joinable())
        im->decodeThread.join();
    if (im->presentThread.joinable())
        im->presentThread.join();

    if (im->fmt)
        avformat_close_input(&im->fmt);
    im->fmt = nullptr;
    if (im->vctx)
        avcodec_free_context(&im->vctx);
    im->vctx = nullptr;
    if (im->actx)
        avcodec_free_context(&im->actx);
    im->actx = nullptr;
    if (im->swr)
        swr_free(&im->swr);
    im->swr = nullptr;
    if (im->hwDeviceRef)
        av_buffer_unref(&im->hwDeviceRef);
    im->hwDeviceRef = nullptr;
    {
        std::lock_guard<std::mutex> lk(im->vqMtx);
        while (!im->vq.empty()) {
            im->freeVFrame(&im->vq.front());
            im->vq.pop_front();
        }
        im->vqBytes = 0;
    }
    {
        std::lock_guard<std::mutex> lk(im->histMtx);
        while (!im->hist.empty()) {
            im->freeVFrame(&im->hist.front());
            im->hist.pop_front();
        }
        im->histBytes = 0;
    }
    {
        std::lock_guard<std::mutex> lk(im->aMtx);
        im->aBuf.clear();
        im->aFront = 0;
        im->aPlayed.clear();
        im->revBuf.clear();
    }
    if (im->sws)
        sws_freeContext(&im->sws);
    im->sws = nullptr;
    if (im->sysFrame)
        av_frame_free(&im->sysFrame);
    im->sysFrame = nullptr;
    {
        std::lock_guard<std::mutex> lk(im->fMtx);
        im->fBuf.clear();
        im->fW = im->fH = 0;
        im->fToken++;
    }
    im->vIdx = im->aIdx = -1;
    im->vst = im->ast = nullptr;
    im->stateV_.store(PlayerState::Stopped);
}

void PlaybackEngine::play() {
    auto *im = p_;
    if (!im->opened.load())
        return;
    if (im->stateV_.load() == PlayerState::Ended)
        im->requestSeek(0.0);
    im->playing.store(true);
    im->stateV_.store(PlayerState::Playing);
}

void PlaybackEngine::pause() {
    auto *im = p_;
    if (!im->opened.load())
        return;
    im->playing.store(false);
    if (im->stateV_.load() == PlayerState::Playing)
        im->stateV_.store(PlayerState::Paused);
}

void PlaybackEngine::togglePlayPause() {
    auto *im = p_;
    if (!im->opened.load())
        return;
    if (im->playing.load())
        pause();
    else
        play();
}

void PlaybackEngine::seekTo(double seconds) {
    auto *im = p_;
    if (!im->opened.load())
        return;
    double d = im->duration();
    if (d > 0)
        seconds = clampD(seconds, 0.0, d - 0.02);
    // The presentation loop shows a preview frame when this seek's
    // generation lands while paused (it reads the fresh target itself).
    im->requestSeek(seconds);
}

void PlaybackEngine::skipBy(double seconds) {
    auto *im = p_;
    if (!im->opened.load())
        return;
    seekTo(position() + seconds);
}

void PlaybackEngine::stepFrame(int dir) {
    auto *im = p_;
    if (!im->opened.load() || !im->info.hasVideo)
        return;
    im->playing.store(false);
    if (im->stateV_.load() == PlayerState::Playing)
        im->stateV_.store(PlayerState::Paused);
    {
        std::lock_guard<std::mutex> lk(im->stepMtx);
        if (im->stepQueue.size() < 8)
            im->stepQueue.push_back(dir);
    }
}

void PlaybackEngine::setSpeed(double s) {
    s = clampD(s, 0.1, 8.0);
    p_->speedV_.store(s);
    auto *im = p_;
    if (!im->opened.load())
        return;
    int rate = 0;
    if (im->info.hasAudio && s >= 0.25 && s <= 4.0)
        rate = (int)(48000.0 * s + 0.5);
    {
        std::lock_guard<std::mutex> lk(im->aMtx);
        im->deviceRateReq = rate;
    }
    im->audioReqType.store(1);
    im->audioReq.fetch_add(1);
}

void PlaybackEngine::setReverse(bool on) {
    auto *im = p_;
    if (!im->opened.load())
        return;
    im->dirV_.store(on ? -1 : 1);
    im->reverseAudio.store(on);
    im->audioReqType.store(on ? 3 : 2);
    im->audioReq.fetch_add(1);
    if (!on)
        im->queueStale.store(true); // frames behind the playhead are not queued
}

void PlaybackEngine::setLoop(bool on) { p_->loopV_.store(on); }

PlaybackEngine::Frame PlaybackEngine::currentFrame() {
    auto *im = p_;
    std::lock_guard<std::mutex> lk(im->fMtx);
    Frame f;
    if (!im->fBuf.empty()) {
        f.rgba = im->fBuf.data();
        f.w = im->fW;
        f.h = im->fH;
        f.pts = im->fPts;
    }
    f.token = im->fToken;
    return f;
}

double PlaybackEngine::position() {
    auto *im = p_;
    if (im->info.hasVideo)
        return im->curPts.load();
    double ac = im->audioClock();
    if (ac > 0)
        return ac;
    return im->curPts.load();
}

double PlaybackEngine::duration() const { return p_->info.duration; }

std::string PlaybackEngine::lastError() const {
    auto *im = p_;
    std::lock_guard<std::mutex> lk(const_cast<Impl *>(im)->errMtx);
    return const_cast<Impl *>(im)->errMsg;
}

void PlaybackEngine::setAudioSink(AudioSink *sk) {
    auto *im = p_;
    im->sink.store(sk);
    im->sink_ = sk;
}

void PlaybackEngine::setVolume(int percent) {
    auto *im = p_;
    AudioSink *sk = im->sink.load();
    if (sk)
        sk->setVolume(percent);
}
