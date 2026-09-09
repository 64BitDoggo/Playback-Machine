// Playback Machine — headless engine smoke test (Linux/CI).
//
// Exercises: open, 1x/2x/0.5x speed, frame stepping (both directions),
// seek, +/-5s skip, reverse playback (including history refill), forward
// resume, end-of-stream, close.  Prints PASS/FAIL per check.
//
//   smoke <file.mp4>

#include "audiosink_null.h"
#include "engine.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

static int failures = 0;

#define CHECK(name, cond)                                             \
    do {                                                              \
        bool ok = (cond);                                             \
        printf("[%s] %-28s", ok ? "PASS" : "FAIL", name);             \
        if (!ok) {                                                    \
            failures++;                                               \
            printf("   | ");                                          \
        }                                                             \
        printf("\n");                                                 \
    } while (0)

#define CHECKF(name, cond, fmt, ...)                                  \
    do {                                                              \
        bool ok = (cond);                                             \
        printf("[%s] %-28s", ok ? "PASS" : "FAIL", name);             \
        if (!ok) {                                                    \
            failures++;                                               \
            printf("   | " fmt, ##__VA_ARGS__);                       \
        }                                                             \
        printf("\n");                                                 \
    } while (0)

using Clock = std::chrono::steady_clock;
static void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 2) {
        printf("usage: smoke <file>\n");
        return 2;
    }
    std::string err;
    if (!PlaybackEngine::ffLoad(&err)) {
        printf("FFmpeg load failed: %s\n", err.c_str());
        return 2;
    }
    printf("== Playback Machine engine smoke test ==\n");

    PlaybackEngine eng;
    NullAudioSink sink;
    eng.setAudioSink(&sink);

    if (!eng.open(argv[1], &err)) {
        printf("open failed: %s\n", err.c_str());
        return 2;
    }
    const EngineInfo &inf = eng.info();
    printf("info: container=%s %dx%d fps=%.2f dur=%.2fs v=%s a=%s hw=%s(%s)\n",
           inf.container.c_str(), inf.width, inf.height, inf.fps, inf.duration,
           inf.videoCodec.c_str(), inf.audioCodec.c_str(),
           inf.hwAccel ? "yes" : "no", inf.hwName.c_str());
    CHECK("has video", inf.hasVideo);
    CHECK("has audio", inf.hasAudio);
    CHECKF("duration > 12s", inf.duration > 12.0, "dur=%.2f", inf.duration);

    // ---- 1x forward -------------------------------------------------
    double p0 = eng.position();
    sleepMs(1500);
    double p1 = eng.position();
    CHECKF("1x advances ~1.5s", p1 > p0 + 1.0 && p1 < p0 + 2.1, "p0=%.3f p1=%.3f", p0, p1);
    CHECK("state=Playing", eng.state() == PlayerState::Playing);
    PlaybackEngine::Frame fr = eng.currentFrame();
    CHECKF("frame displayed", fr.w > 0 && fr.h > 0 && fr.rgba != nullptr && fr.pts > 0.01, "w=%d h=%d pts=%.3f", fr.w, fr.h, fr.pts);
    // ---- 2x ----------------------------------------------------------
    eng.setSpeed(2.0);
    sleepMs(200);
    p0 = eng.position();
    sleepMs(1000);
    p1 = eng.position();
    CHECKF("2x advances ~2s", p1 > p0 + 1.4 && p1 < p0 + 2.8, "p0=%.3f p1=%.3f", p0, p1);

    // ---- 0.5x --------------------------------------------------------
    eng.setSpeed(0.5);
    sleepMs(200);
    p0 = eng.position();
    sleepMs(1000);
    p1 = eng.position();
    CHECKF("0.5x advances ~0.5s", p1 > p0 + 0.25 && p1 < p0 + 0.95, "p0=%.3f p1=%.3f", p0, p1);
    eng.setSpeed(1.0);
    sleepMs(200);

    // ---- frame step forward (x5) ------------------------------------
    eng.pause();
    sleepMs(250);
    double pa = eng.position();
    for (int i = 0; i < 5; ++i) {
        eng.stepFrame(+1);
        sleepMs(200);
    }
    double pb = eng.position();
    double stepDur = inf.fps > 0 ? 1.0 / inf.fps : 1.0 / 30.0;
    CHECKF("step +5 frames", pb > pa + 5 * stepDur * 0.6 && pb < pa + 5 * stepDur * 1.4 + 0.1, "pa=%.4f pb=%.4f expect~%.4f", pa, pb, 5 * stepDur);
    CHECK("paused after step", eng.state() == PlayerState::Paused);
    // ---- frame step backward (x10) ----------------------------------
    double pc = eng.position();
    for (int i = 0; i < 10; ++i) {
        eng.stepFrame(-1);
        sleepMs(200);
    }
    double pd = eng.position();
    CHECKF("step -10 frames", pc - pd > 10 * stepDur * 0.6 && pc - pd < 10 * stepDur * 1.4 + 0.1, "pc=%.4f pd=%.4f expect~%.4f", pc, pd, 10 * stepDur);
    // ---- seek to 15s -------------------------------------------------
    eng.seekTo(15.0);
    sleepMs(1000);
    double ps = eng.position();
    CHECKF("seek to 15s", ps >= 14.9 && ps < 15.6, "pos=%.3f", ps);

    // ---- reverse 1x --------------------------------------------------
    eng.play();
    sleepMs(400);
    eng.setReverse(true);
    sleepMs(400);
    CHECK("reverse active", eng.isReverse());
    double ra = eng.position();
    sleepMs(1200);
    double rb = eng.position();
    CHECKF("reverse ~1.2s back", ra - rb > 0.8 && ra - rb < 1.7, "ra=%.3f rb=%.3f", ra, rb);

    // ---- reverse keeps going (history refill) ------------------------
    sleepMs(3000);
    double rc = eng.position();
    CHECKF("reverse refill works", rc < rb - 0.5, "rc=%.3f rb=%.3f", rc, rb);

    // ---- back to forward ---------------------------------------------
    eng.setReverse(false);
    sleepMs(600);
    CHECKF("forward resumed", eng.position() > rc, "pos=%.3f rc=%.3f", eng.position(), rc);
    CHECK("not reversing", !eng.isReverse());
    // ---- skip +5 / -5 --------------------------------------------------
    double sa = eng.position();
    eng.skipBy(5.0);
    sleepMs(800);
    CHECKF("skip +5s", eng.position() > sa + 4.0, "sa=%.3f pos=%.3f", sa, eng.position());
    double sb = eng.position();
    eng.skipBy(-5.0);
    sleepMs(800);
    CHECKF("skip -5s", eng.position() < sb - 4.0, "sb=%.3f pos=%.3f", sb, eng.position());

    // ---- loop to end ---------------------------------------------------
    eng.seekTo(inf.duration - 0.4);
    sleepMs(900);
    PlayerState st = eng.state();
    CHECKF("reaches end", st == PlayerState::Ended || eng.position() > inf.duration - 0.6, "state=%d pos=%.3f", (int)st, eng.position());
    // ---- close ----------------------------------------------------------
    eng.close();
    CHECK("closed cleanly", eng.state() == PlayerState::Stopped);
    if (failures == 0)
        printf("ALL TESTS PASSED\n");
    else
        printf("%d TEST(S) FAILED\n", failures);
    return failures ? 1 : 0;
}
