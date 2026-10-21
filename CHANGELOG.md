# Changelog

## 1.0.1 (TU2) — 2026-10-21

Release build and distribution. No ready-made NRO is published any more: the release archive holds the prepared shader
library and cache and the game-file script, and the NRO is built from source (see [INSTALL.md](INSTALL.md)).

- Debug shortcuts are off: only **ZL + ZR + D-pad Up** (FPS counter, hidden at start) answers; the rest need
  `switch_atajos_depuracion=true`.
- Our own Homebrew Menu icon, with no art from the game (`-DDS2_ICON_DEL_DISCO=ON` uses your disc's).
- `tools/stage_sd.py` reads the title update package itself: only Python and your own files are needed.
- `build_mac.sh` fetches Dead-Space-2-Recomp at the commit this release was tested with, and runs on Linux too
  (untested there).

## 1.0.0 (TU2) — 2026-10-21

First release: Dead Space 2 is playable on the Switch, natively (homebrew NRO). Tested through the early chapters,
the decompression scene, a space walk and a boss fight; a full playthrough has not been verified yet.

### Rendering

- Native deko3d renderer for the game's deferred lighting: G-buffer, light buffer, shadows, particles, post-processing
  and the HUD and menus (including their Scaleform panels), from the game's own shaders translated ahead of time.
- Every shader variant is precompiled (`shadercache_dksh.bin`): no compile stalls during play.
- Fixed in this cycle: streaks and broken triangles (vertex index and computed vertex fetches), missing menu and
  objective-path panels (stencil of Direct3D clears), the decompression scene losing its whole G-buffer, smoke piling
  up in the quarter-size particle buffer, garbage and vertical strips in the boss fight and the space walks
  (sub-rectangle and narrow resolves).

### Performance

- Overclocked: comfortable, close to the game's 30 fps cap.
- Stock clocks (CPU 1020 MHz, GPU 307 MHz): about 20-30 fps in most areas, 12-16 fps in the heaviest fights.
- The recompiled code's "interrupts off" regions no longer serialize every thread on one mutex (the job scheduler
  blocked the main thread up to a quarter of its time).
- Audio: the mixer no longer waits for XMA decodes, its hottest loops run on NEON, and the final compressor takes its
  own bypass.

### Controls

The game uses the controller as on the Xbox 360. Debug shortcuts hold both rear triggers (ZL + ZR): Up the FPS
counter, Down a screenshot, Left a frame dump and trace, Right the draw-skipping modes, R CPU stack sampling, L the GPU
A/B lap (in 1.0.1 only the FPS counter is on by default).

### Known issues

- Shadows show some self-shadowing bands and blotches (shadow acne) on characters and furniture.
- At stock clocks the heaviest scenes drop to 12-16 fps; the light ones are limited by the 307 MHz GPU (~28 fps).
- At stock clocks the intro video (and the other full-motion videos) stutters: the game decodes VP6 video in software
  on one thread, which needs more than one core at 1020 MHz. The screen still updates at 30 fps; overclocked the videos
  play nearly smoothly.
- The final mix has no dynamics compression (`ds2_audio_sin_compresor=false` restores it, at a CPU cost).
