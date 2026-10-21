# Changelog

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

The game uses the controller as on the Xbox 360. **ZL + ZR + D-pad Up** shows or hides the FPS counter (hidden at
start). The debugging shortcuts (screenshots, frame dumps, draw-skipping modes, the SDK menus, CPU stack sampling) are
off; `switch_atajos_depuracion=true` in the configuration turns them back on.

### Other

- The Homebrew Menu icon is our own, with no art from the game (a personal build can use your disc's with
  `-DDS2_ICON_DEL_DISCO=ON`).
- `tools/stage_sd.py` reads the title update package itself: only Python and your own files are needed.

### Known issues

- Shadows show some self-shadowing bands and blotches (shadow acne) on characters and furniture.
- At stock clocks the heaviest scenes drop to 12-16 fps; the light ones are limited by the 307 MHz GPU (~28 fps).
- The final mix has no dynamics compression (`ds2_audio_sin_compresor=false` restores it, at a CPU cost).
