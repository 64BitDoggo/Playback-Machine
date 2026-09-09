#!/usr/bin/env python3
"""Generate a 20 s test video (640x360 @ 30 fps, RGB) + 48 kHz stereo PCM.

Each frame has a unique color in a top strip and a moving bar, so frame
stepping / reverse playback is easy to verify visually.
"""
import numpy as np
import sys

W, H, FPS, DUR = 640, 360, 30, 20
n = FPS * DUR
rgb_path = sys.argv[1] if len(sys.argv) > 1 else "test_raw.rgb"
pcm_path = sys.argv[2] if len(sys.argv) > 2 else "test_raw.pcm"

with open(rgb_path, "wb") as frgb, open(pcm_path, "wb") as fpcm:
    t = np.arange(W)[None, :] / W  # 1 x W
    for i in range(n):
        f = i / FPS
        # moving vertical bar
        bar = (np.abs(t - (f % 1.0)) < 0.02).astype(np.uint8)  # 1 x W
        # unique hue per frame for a top strip (16 px)
        strip = np.zeros((16, W, 3), np.uint8)
        strip[:, :] = ((i * 7) % 256, (i * 13) % 256, (i * 29) % 256)
        frame = np.full((H, W, 3), 40, np.uint8)
        frame[:, :] += (bar * 200).astype(np.uint8)[:, :, None]
        frame[:16] = strip
        # checkerboard quadrant that changes every 5 s
        q = ((i // (FPS * 5)) % 2)
        yy = np.arange(H)[:, None]
        xx = np.arange(W)[None, :]
        chk = ((yy // 45 + xx // 45) % 2).astype(np.uint8)  # H x W
        if q:
            frame[16:, :] = (frame[16:, :].astype(np.int32) + chk[16:, :, None] * 25).astype(np.uint8)
        frgb.write(frame.tobytes())
        # audio: 440 Hz tone; 48000 Hz stereo -> 1600 samples per 30 fps frame
        NS = 48000 // FPS
        tt = np.arange(NS) / 48000.0
        amp = 0.6 * (1.0 if (i // FPS) % 2 == 0 else 0.3)
        tone = (amp * 0.9 * np.sin(2 * np.pi * 440 * (f + tt))).astype(np.int16)
        stereo = np.empty((NS, 2), np.int16)
        stereo[:, 0] = tone
        stereo[:, 1] = (tone * 0.7).astype(np.int16)
        fpcm.write(stereo.tobytes())
print("wrote", rgb_path, pcm_path)
