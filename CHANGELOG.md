# Changelog

## 1.2.2 (TU2) — 2026-10-25

The game's own brightness and vibration settings now work. Only the NRO changes.

- **Brightness setting works**: the game sets its brightness as a gamma ramp (`Displ_SetGamma` ->
  `D3DDevice_SetGammaRamp`), which the Xbox 360 applied in its display output and the Switch's output ignored. The
  ramp Direct3D keeps after that call now goes to the renderer, which applies it to the presented image
  (`nfsmw_nativo_rampa_gamma = false` turns it off). The picture also gets the Xbox 360's own output gamma.
- **Vibration setting works**: the SDK sent the controllers zero vibration unless `input_vibracion` was on, so the game's
  vibration option changed nothing. It is on by default now; the game's own setting turns vibration on and off
  (`input_vibracion = false` in `ds2.toml` still silences it).

## 1.2.1 (TU2) — 2026-10-25

Fixes from players' logs. Only the NRO changes: the shader library and its cache stay as they are.

- **Crash right after waking from sleep** (abort, "la cola de deko3d esta en error"): an achievement unlocked after
  waking opened the SDK's achievement toast, whose ImGui dialog moves presentation to the UI thread, and the deko3d
  queue went into error. The toast is off, as in ds3-nx; achievements still unlock and are saved.
- **Stuck on "insert disc 2"** with disc 2 on the SD card: at chapter 4 the game waits for its stream manager to go
  idle before it swaps discs, and on a player's console it never did, so `XamSwapDisc` was never called. The wait is
  logged (`DS2_DISC_WAIT_STREAMS`, with the stream manager's two queues) and after 10 s
  (`ds2_espera_streams_disco_s`) the swap goes on anyway.
- **RenderWare Audio stream pool list**: a broken link at the head now empties the list (after a 44-minute sleep the
  head itself was a freed pool, and cutting after it still crashed).

## 1.2.0 (TU2) — 2026-10-24

**Playable at stock clocks.** Without overclocking (handheld, CPU 1020 MHz) the game now holds 29-30 fps in most
places at 540p; the heaviest scenes (~3,000 draws) still drop to about 20 fps, bound by the GPU. Most of this comes from
the Dead Space 3 port (ds3-nx); details in [PORTING.md](deadspace2/PORTING.md) 5.6.

**Resolution menu.** While playing, press **ZL + ZR + Minus**: an options menu opens over the game (the game ignores
the controller while it is open). **Up / Down** choose **720P** (1280x720, for overclocked consoles) or **540P** (960x540,
for stock clocks), **A** saves, **B** closes. The choice is kept in `ds2_resolucion.txt` next to the NRO and applies at
the **next start** of the game. Without a saved choice: `ds2_resolucion` in `ds2.toml`, else 540p below a 1.5 GHz CPU
and 720p above.

- **Handheld GPU at 460.8 MHz** through Horizon's own performance profile (the one commercial games use; CPU and memory
  clocks unchanged, not an overclock). `ds2_gpu_portatil_mhz = 0` keeps the stock 307.2 MHz.
- **960x540 scene at stock clocks**, scaled to the 1280x720 screen: about half the pixel work. **ZL + ZR + Minus** opens an
  options menu over the game to choose 720p or 540p (saved in `ds2_resolucion.txt`, applied at the next start). Without
  a saved choice: `ds2_resolucion` in `ds2.toml`, else by the CPU clock (720p from 1.5 GHz up).
- **Direct3D's wait for the GPU no longer spins a core**: it yields, then sleeps 100 µs.
- **Audio threads above the game's** (`ds2_prioridad_mezclador`, 0x2E): RenderWare Audio's mixer threads and XAudio2's
  processing thread, so the sound keeps up when the game keeps the cores busy (Dead Space 3 lost up to a third of its
  sound frames in fights at stock clocks without it).
- **SDK**: partial GPU submits (the GPU starts drawing before the frame is fully recorded), a lock-free guest clock,
  the geometry hash inlined, resolved texture sizes kept by recency, and the overlay text and controller filter the
  menu uses (`sdk-patches/0008`-`0012`).
- **540p rendering fixed (from ds3-nx's run 16)**: a Direct3D color clear no longer clears depth through c1-c3 targets
  its pixel shader does not write (at 960x540 the game's EDRAM layout put them on the scene depth, and the scene came
  out see-through, textures over the wrong surfaces; `ds2_borrado_solo_salidas = false` restores the old rule), and
  depth resolves read smaller or as a stack of slices are cropped or stacked (`sdk-patches/0014`). Confirmed on the
  console.
- **RenderWare Audio crash** (the mixer following a broken stream pool link: after sleep and on some loads): the list is
  checked by its back links before each walk and cut at a broken link instead of followed, logged as `[audio] lista de
  pools`; `ds2_audio_lista_pools_segura = false` turns it off. Not yet confirmed after sleep on the console.
- The log is written to the SD when the console sleeps, goes HOME or resumes (`sdk-patches/0013`): a crash on waking
  no longer leaves a log that ends minutes earlier.
- `tools/build_mac.sh` applies the SDK patch series as a whole (several touch the same files) and records it in
  `deadspace2/out/sdk-patches.stamp`; an SDK patched by an older version must be reset first
  (`git -C sdk checkout -- .`).

## 1.1.0 (TU2) — 2026-10-23

- **Faster recompiled code.** Guest registers now live in host locals inside each function, and the context is written
  back only at calls and returns, for the registers a whole-program analysis says the other side reads
  (`deadspace2/tools/patch_local_regs.py`, ported from gears2-nx 1.0.0; [PORTING.md](deadspace2/PORTING.md) 5.5).
  The NRO shrinks from 49 to 39 MB. Videos no longer stutter at stock clocks. In the heaviest scenes at stock the GPU
  is the limit, so their frame rate is unchanged.
- **No debug modes in the release.** Only ZL + ZR + D-pad Up (FPS counter) answers; `switch_atajos_depuracion` in
  `ds2.toml`, `telemetria.flag` and `sin_resplandor.flag` are ignored. Building with `-DDS2_DEPURACION=ON` brings them
  back for development.
- Tested on the console for about 12 minutes at stock clocks (no crash); a full playthrough is not verified.

## 1.0.5 (TU2) — 2026-10-22

- **Crash after waking from sleep.** While the console slept, the game's clock kept running, and on waking the game took
  the whole nap as one frame: RenderWare Audio's mixer then crashed within seconds (a broken stream pool list). The
  guest clock no longer advances while the process is suspended: any gap longer than 0.25 s counts as 0.25 s
  (`sdk-patches/0007-guest-clock-sleep.patch`). Not yet confirmed on the console.
- Known issue: the same audio crash was also seen once on a loading screen, without sleep; if it comes back, it needs
  its own fix.

## 1.0.4 (TU2) — 2026-10-21

- **The Russian edition is supported** (Dead Space 2 (Russia) (En,Ru), Media ID 2C199AEB, TU2
  `TU_12K226V_000000K000000.00000000000G5`). After TU2 its executable recompiles to byte-identical code with the World
  edition's, so the same NRO runs both; `tools/stage_sd.py` accepts either edition's discs and update.
- Disc and update must come from the same edition: a mismatched pair is refused with the Media ID of the TU2 the disc
  needs.
- The game takes its language from the console's: for Russian, put `ds2.toml` with `user_language = 12` next to the
  NRO ([INSTALL.md](INSTALL.md)).
- Not yet played through on a Switch with the Russian edition.

## 1.0.3 (TU2) — 2026-10-21

The public release: nothing from the game is distributed or committed any more.

- **No prebuilt files.** The shader library and its cache are made by each player from their own copy: without
  `ds2_shaders.dslib` next to it, the first start writes every shader the game creates to `shaders_dump/`, and
  `shaders/build_library.sh` turns them into the library on the computer ([INSTALL.md](INSTALL.md), step 3).
- The Homebrew Menu icon is ours again; a build from your own disc can use the game's (`tools/make_icon.py`,
  `-DDS2_ICON_DEL_DISCO=ON`).

## 1.0.2 (TU2) — 2026-10-21

- The Homebrew Menu icon is the game's again (Isaac's helmet from the disc's dashboard art, `deadspace2/res/icon.jpg`).
  `-DDS2_ICON_NEUTRAL=ON` builds with the neutral one.

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
