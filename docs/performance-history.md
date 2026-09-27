# Performance history

How a race went from 1-3 FPS to 30 FPS and above at the Switch's stock clocks, in the order it happened. All the
numbers are from the console, in handheld mode, during the same race used for every test. The reasoning behind each
step is in the other documents; this page is the timeline.

Stock clocks for a game: CPU 1020 MHz (three cores for the game), GPU 307.2 MHz in handheld mode (460.8 MHz with the
performance configuration described below) and 768 MHz docked, memory 1331.2 MHz in handheld mode.

## 1. The emulated GPU: 1-3 FPS

The first builds ran the game with the SDK's own GPU backend: the Xenos command stream translated at run time, the
EDRAM emulated, shaders converted on first use. The game logic, menus and audio worked; a race ran at 1-3 FPS. The
CPU cost of emulating every draw was far beyond what three 1 GHz cores can do, so the decision was to replace the
GPU emulation with a native renderer instead of optimizing it ([native-renderer.md](native-renderer.md)).

## 2. The native renderer

The renderer consumes the PM4 ring the game's Direct3D writes and records Vulkan on its own thread. It was developed
on the PC first: in two days it drew videos, menus, the HUD and full races with the rear-view mirror, reflections
and shadows, at 1.5-1.8 us of CPU per draw.

The first console numbers looked like 22-28 FPS, but they had been taken with the console overclocked. The first
measurement at stock clocks, a few days later, is the real starting point: **16.1 FPS median**, with the GPU busy
for the whole frame:

| GPU time per frame | ms |
|---|---|
| Scene | 27.2 |
| Shadow maps | 13.6 |
| Copies | 7.4 |
| Cubemap and blur | 7.0 |
| Post-processing | 3.9 |
| **Total** | **60.2** |

The CPU was not saturated (about 200 % of the 300 % available). The frame was GPU bound, and the GPU was spending
its time shading fragments, not on bandwidth.

## 3. GPU work from 60 to 33 ms

The goal was 30 FPS at stock clocks, keeping the look of the Xbox 360 version. The GPU work came down in steps:

- **One pass instead of the Xbox 360 tiling.** The Xbox 360 draws the scene in tiles to fit its 10 MB of EDRAM, with
  MSAA. The Switch does not need either; the scene is drawn once.
- **Shader constants through a dynamic uniform buffer**, and the **inverse texture size** computed once instead of
  160 size queries ([shaders.md](shaders.md)).
- **Resolves without copies**: swapping images instead of copying render targets saved 2.36 ms.
- **ZCULL** enabled in NVK: about 2 ms of the scene ([mesa.md](mesa.md)).
- **Shadow maps**: a cheaper 3x3 PCF (the nine samples of a filter that lands on one texel collapse to one), no
  vegetation in the shadow maps, and a shadow pass that does not load the previous contents.
- **Predicated blocks** of the translated shaders merged, which removed most of their 1806 branches, and redundant
  `max(a, a)` removed.

By the end of this stage the GPU work fit in 33.35 ms, and a race averaged **26.0 FPS** over nine minutes. It did
not reach 30 because the GPU sat idle for several milliseconds per frame waiting for work.

## 4. Frame pacing

Two findings changed how frames reached the screen, independently of the frame time:

- **Half of the frames were thrown away.** An invisible achievements dialog registered itself as a UI drawer at
  startup; with one drawer registered, the presenter painted from the UI thread and coalesced frames. Every second
  rendered frame never reached the screen. With that fixed, every rendered frame is presented.
- **A presentation thread of its own** gained 1.5 FPS on average but made the frame time range three times wider and
  the frames over 50 ms 2.4 times more frequent. It is off: a steady frame time feels better than a higher average
  ([measuring.md](measuring.md)).

Output is quantized to the display's refresh: a 34 ms frame is shown for 50 ms. That is why frames just over 33.3 ms
matter so much, and why the distribution of frame times is the number to watch.

## 5. The CPU becomes the limit

With the GPU work under 33 ms, the frame was limited by two CPU threads: the ring thread, which records Vulkan, and
the game's main thread. The next weeks went to the CPU cost per draw in the ring thread (see
[native-renderer.md](native-renderer.md) for the list): caches for immediate loads, deduplicated uploads, far fewer
query pool resets (1.2 ms), a swapchain format check that recreated the composition 6,000 times, no logging from the
ring thread, and the game's own busy waits in its Direct3D layer replaced by real waits.

At the same time the renderer asks the system, through `apm`, for the stock performance configuration `0x92220008`:
GPU at 460.8 MHz with memory at 1331.2 MHz in handheld mode (see [platform-notes.md](platform-notes.md) for the
pitfalls of setting it). With it and the sky dome drawn later in the frame (about 3 ms), the scene went from 22.3 to
about 12 ms and a race reached **30.3 FPS**, now limited by the
CPU (the three cores at 90, 99 and 93 %).

## 6. The build

The recompiled code is about 144 MB of C++. Compiling it for the target instead of for the PC paid off in three
steps ([toolchain.md](toolchain.md)):

- **Direct calls** between recompiled functions instead of going through the function table, which lets the
  compiler see and inline across them.
- **LTO** over the recompiled code and the app.
- **PGO** with a profile recorded on the console, and **function ordering** to keep the hot code together.

With these, a race averaged **about 34 FPS**.

## 7. Native game functions

The game's main thread spends much of its time in its own renderer front end (materials, effect parameters,
visibility, matrices), which the recompiled code runs far slower than native code would. The hottest of those
functions were rewritten in C++, each behind a guard that runs both versions and compares them before trusting the
native one, and keeps checking afterwards ([native-renderer.md](native-renderer.md)):

- material parameters (about 2-4 % of the game thread) and effect parameters (about 5 %);
- object visibility (82,000 calls per second);
- per-draw matrices (595 instructions down to 394, 265 memory accesses down to 78) and the view render loop;
- the Direct3D state of each draw passed to the ring as one marker instead of about 14 packets.

Result: **35.4 FPS** average in a race, the heaviest alley from 27.6 to 30.0 FPS, and frames of 60 ms or more cut
from about 24 to 7 in the test race (-70 %).

## 8. Where it stands

The work after that moved the GPU time per frame down to about 23 ms and cut the per-draw cost inside NVK
([mesa.md](mesa.md)); from there on the ring thread sets the pace, and every millisecond of GPU saved is worth about
half a frame per second. The released build averages 32 to 35 FPS in races depending on the area. The heaviest place
measured is the exit of Heritage Heights, about 3,000 draws per frame, which runs at 21-22 FPS for its first 20
seconds or so.

## Lessons

- The overclocked numbers of the first days were the most expensive mistake: they hid for days that the frame was
  GPU bound. Measure at stock clocks from the start.
- A configuration file that overrides code defaults can keep a change switched off for several builds without anyone
  noticing. Check what is actually running, from the log.
- One millisecond of GPU is not one millisecond of frame: when CPU and GPU overlap, a GPU saving only shows up in
  half.
- The average hides what the player feels. Look at the frames over 33.3 and 50 ms.