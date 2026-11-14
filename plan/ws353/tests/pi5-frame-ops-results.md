# Raspberry Pi 5: desktop frame rate (frame-ops), 2026-11-14

Board: Pi 5 at 10.0.30.2, main 024d9ef56f userland (WS353 complete). Kernel for the run: `plan/tools/aat/config-rpi5-aat.mk`
built from af416b2be3 (no source difference from 024d9ef56f in src/, include/, userland/, config/, mk/), so the only
kernel change is the test input injector. Session script with `--log-frames` and `TERMINAL_PERF=1` for the run only.
Output 1920x1080, refresh 60.000 Hz (`KWL DISPLAY ... width=1920 height=1080 refresh_mhz=60000`).
Script: [pi5-frame-ops.sh](pi5-frame-ops.sh) (deploy, run, restore). QEMU not used; this is real-hardware evidence only.

| step | frames | fps | work p50/max ms | period p50/max ms | lag p50/max ms |
|---|---|---|---|---|---|
| launch (Terminal from App Home) | 48 | 58 | 1/10 | 17/34 | 18/3068 |
| type | 61 | 60 | 1/3 | 17/18 | 1/3 |
| keys (200 ms apart) | 23 | 75 | 1/1 | 13/13 | 1/1 |
| output | 11 | 61 | 1/1 | 16/19 | 1/1 |
| scroll | 40 | 19 | 1/1 | 53/55 | 1/1 |
| hover | 15 | 21 | 1/2 | 36/91 | 2/3 |
| drag | 191 | 60 | 1/2 | 17/33 | 8/18 |
| App Home | 67 | 61 | 1/5 | 17/27 | - |
| Wiseview | 57 | 59 | 2/13 | 17/60 | - |

Key to screen (22 keys, ms from the seat, median/max): client 0.0/0.1, term 0.2/0.4, write 0.3/0.5, read 0.6/0.8,
present 1.3/1.7, adopt 3.3/4.2, shown 17.2/25.0. Since WS353 `shown` is the output's present SHOWN, not the GPU's
completion, so it is not directly the VF2 baseline's 30 ms.

Against the VF2 baseline (drag 60, Home 58-60, Wiseview 58-61, launch 56-60, key 30 ms): equal or better in every step.

Below 58 fps:
- scroll 19 and hover 21 are paced by the input: one frame per wheel notch (frame-ops gives one each 50 ms plus the
  aat-input call), and on hover a frame only when a highlight changes (the pointer itself moves on the cursor plane,
  without a composed frame). Work stays 1-2 ms.
- launch's period max 34 and lag max 3068: frame 79 is the first after the idle step (the lag is the gap since the
  idle step's last input), frames 80 and 105-107 are the App Home to Terminal change (one 6.2 Mpixel frame, a 10 ms
  cpu frame), each one missed vblank. Wiseview's 60 ms is the still frame after its last close; its first open misses
  one vblank (frame 537: 12 ms cpu, period 28).
