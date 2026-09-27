# Horizon and NVK: platform notes

Facts about the Switch, Horizon OS, libnx and the NVK Vulkan driver that cost time to find out. They
apply to any statically recompiled Xbox 360 game running through the ReXGlue runtime and Mesa on the
Switch, not only to this one.

## Hardware at stock clocks

Always optimise against stock clocks. The numbers a game really gets:

| | CPU | GPU | Memory |
|---|---|---|---|
| Docked | 1020 MHz | 768 MHz | 1600 MHz, 25.6 GB/s |
| Handheld | 1020 MHz | 307.2 MHz for this port | 1331.2 MHz, 21.3 GB/s |

- Handheld mode has three GPU steps (307.2, 384 and 460.8 MHz) and each title gets its own. A
  homebrew application gets 307.2 MHz. Profiles of commercial games that report 460.8 MHz are not the
  budget.
- **Three of the four CPU cores** are available to the application (core mask 0x7).
- The GPU is a GM20B (Maxwell): 2 SMs, 256 cores, 16 ROPs and TMUs, **256 KB of L2**, 12 KB of L1
  and more than 400 ns of memory latency. Memory bandwidth is shared with the CPU, so vertex copies
  on the CPU compete with the GPU.
- It is an **immediate-mode** GPU like every NVIDIA part, not a tiler. Render pass load and store
  operations barely matter; what costs is work per fragment and per vertex. Full-screen passes are
  expensive because nothing fits in 256 KB of L2.
- FP16 runs at twice the FP32 rate. Shaders translated from Xbox 360 microcode do not use it.
- Erista and Mariko run at the same stock clocks. Erista heats up sooner in long sessions, so a
  frame-time margin tested on Mariko is not a margin on Erista.
- An application gets about 3.2 GB. This port runs at 3185 of 3189 MB, so there is no room for
  "one more buffer" solutions: each extra GPU work slot of the renderer costs 64 MB.

## Guest memory

- Horizon limits how much memory a process may **map**, separately from free RAM
  (`LimitableResource_Memory`, readable with `svcGetInfo(InfoType_ResourceLimit)`). When it runs out,
  even a 4 KB commit fails with `0xCE01` (2001-0103, `KernelError_ResourceExhausted`, not the
  out-of-memory code).
- The Xbox 360 sees the same physical memory through up to five views (0x7F000000, 0xA0000000,
  0xC0000000, 0xE0000000 and the raw physical window). Mapping every committed chunk into every view
  turned 472 MB of backing memory into 1782 MB of mapped memory, and the game went black after the
  first logo. Changing the heap size did not move the failure point.
- What works: commit physical memory on demand in 4 MB chunks and map a chunk into a view only when
  that view first touches it, from the exception handler.
- `svcSetProcessMemoryPermission` is refused on memory mapped with `svcMapProcessMemory`, so
  write-watching guest pages has to fall back to unmapping them.

## Exceptions

- libnx has a single exception stack and a single exception dump for the whole process. With about
  27 threads and 30 to 2,500 page faults per second, two threads regularly enter the handler at the
  same time. The visible symptom was a crash in `mutexLock` with a mutex pointer of 1; the invisible
  one was worse: **a thread resumed with the registers saved by another thread**, which corrupts
  guest state without leaving a trace. A probe that only checked that the process stayed alive
  "proved" there was no problem.
- The fix is a pool of stacks and dumps (eight here). The entry code claims a slot with
  `ldaxr`/`stlxr`, keeps the slot number in the dump and releases it on the resume path.
- Any global state touched by the exception path needs the same review. In the first profile of the
  game on the console, the fault path itself (exception entry, handler and a global guest-memory mutex
  taken three times per fault) accounted for most of a hot thread's time.

## Threads

- **`std::thread::detach()` can throw** `std::system_error` on Horizon (seen with libnx 4.12 when the
  thread had already finished), and `std::terminate` closes the game. Use persistent worker threads
  with a queue instead of detached one-shot threads, or catch the exception.
- **Only priority 0x3B is time-sliced** (about 10 ms slices). Every other band is cooperative: a busy
  thread keeps its core against threads of the same or lower priority, **even while other cores are
  idle**. A presentation thread at the same priority as the render thread spent 998 ms of wall time
  per second to get 300 ms of CPU, and the share of frames above 50 ms went from 8 % to 48 %.
- When a thread starves, lower the priority of the one that hogs the core rather than raising the
  starving one. Raising it pushes it into bands that other subsystems depend on.
- Priority bands used by this port (lower number means higher priority):

  | Priority | Threads |
  |---|---|
  | 0x2A | profiler |
  | 0x2B | audio worker, XMA decoder and audio output |
  | 0x2C | presentation and other host threads |
  | 0x2D | GPU command ring of the native renderer |
  | 0x3B | guest threads and bulk work (the only time-sliced band) |

- **A preferred core does not pin a thread.** Horizon accepts the setting and keeps migrating the
  thread (0.24 migrations per ring loop, wherever it was set). An exclusive core mask does pin it, but
  a render thread locked to one core fell short at peaks: the minimum dropped to 16.3 FPS against
  about 21 without pinning. Migrations themselves are cheap (about 0.05 % of CPU).
- **Decide to sleep or wake another thread only while holding the lock.** A vertex copy thread hung
  twice because each side stored its own atomic and then read the other's without a lock
  (Dekker-style). One in millions of wake-ups was lost. Sequentially consistent atomics are not
  enough on ARM either. Every new wait has a timeout and logs how long it has been waiting, so a lost
  wake-up shows as a log line and not as a silent hang.
- **Recompiled games spin.** Two busy-wait loops in the game's Direct3D layer (waiting for the GPU to
  read the command ring, and a frame hand-off polling a flag with `Sleep(0)`) kept two hardware
  threads close to 100 % doing nothing. Hooking the polling functions with bounded sleeps plus
  explicit wake-ups, and calling the original afterwards so the game's own checks still run, brought
  them to 25 % and 17 % on the PC. When a hook reads guest memory, apply the same address offset as
  the recompiled loads (on the PC build, addresses at or above 0xE0000000 are shifted by 0x1000).

## Clocks

- `apmSetPerformanceConfiguration` returns before the clocks change. Reading the clock on the next
  line shows the old value.
- `pcv` reimposes the memory clock of the active performance configuration, so memory clock changes
  made directly through `clkrst` are accepted and then silently reverted.
- Configuration IDs are opaque; read the real table. For example 0x92220007 is GPU 460.8 MHz with
  memory at 1600 MHz and 0x92220008 is GPU 460.8 MHz with memory at 1331.2 MHz. 0x92220006 does not
  exist.

## NVK on Horizon

This port uses the Switch port of Mesa 26.2.1 by danfromtico, with the changes described in
[mesa.md](mesa.md). Behaviour of that driver on the console:

- **Timestamps** advance one unit every 1.627 ns while `timestampPeriod` says 1 ns. See
  [measuring.md](measuring.md).
- **Memory types:** type 0 is `HOST_CACHED` (NvMap with CPU cache; publish writes with
  `vkFlushMappedMemoryRanges`, which cleans the data cache), type 1 is `HOST_COHERENT` (NvMap without
  CPU cache).
- **Fences cost an ioctl** every time they are queried, even when the GPU has already finished.
  Every `vkGetFenceStatus` and `vkWaitForFences` is a round trip.
- **Every `vkQueueSubmit` is an IPC call** (channel kickoff), and the WSI adds one empty submission per
  present. Splitting the scene into two submissions cost 3.6 ms of GPU idle time per frame. Keep the
  number of submissions per frame low.
- **GPU allocations are slow.** Each allocation is a 64 KB-aligned block from the process heap wrapped
  with `nvMapCreate`. Creating one texture with a dedicated allocation cost about 1.9 ms of CPU:

  | Call | Median | Calls per texture |
  |---|---|---|
  | `nvMapCreate` | 422 µs | 1 |
  | `nvAddressSpaceAllocFixed` | 127 µs | 2 |
  | `MapBufferEx` | 626 µs | 2 |

  Dedicated allocations pay for address space and mapping twice, and the only benefit (compression)
  never applies to images with `SAMPLED | TRANSFER_DST` usage. Sub-allocating textures from 32 MB
  slabs brought the cost to 0.75 ms per texture and a burst of 15 new textures in one frame from 28.5
  to 11.3 ms. `requiresDedicatedAllocation` is never true on this driver, and image sizes are already
  rounded to 64 KiB for sub-allocators.
- **The number of allocations is limited, not their size.** libnx gives nvdrv 8 MB of transfer memory
  (`__nx_nv_transfermem_size`), enough for roughly 4,000 NvMap handles. When it is exhausted, creation
  fails with `0x235C` (`LibnxNvidiaError_SharedMemoryTooSmall`), which looks like an out-of-memory
  crash but is not: the game died with 3,585 cached textures and about 600 MB in use. The symbol is
  weak, so the application can raise it before Mesa calls `nvInitialize()`. The driver's buffer object
  cache (`NOUVEAU_HORIZON_BO_CACHE_MB`, 128 MB by default) is limited by its 256 entries, not by
  megabytes.
- **Environment variables are read with `getenv`** when the instance and the devices are created, so
  they have to be set before creating the Vulkan instance. Useful ones: `MESA_SHADER_CACHE_DISABLE`,
  `NVK_SWITCH_PERF_LOG`, `NVK_SWITCH_CMD_MEM_CPU_UNCACHED`, `NVK_SWITCH_MEM_STREAM_CPU_UNCACHED`,
  `NOUVEAU_HORIZON_BO_CACHE_MB`. `MESA_VK_ENABLE_SUBMIT_THREAD` has no effect.
- **Mesa's disk shader cache** writes to `sdmc:/.mesa` from `disk$` threads with 8 MB stacks. If the
  application keeps its own `VkPipelineCache`, the disk cache only duplicates it; disable it with
  `MESA_SHADER_CACHE_DISABLE`. Both caches are invalidated by changing the compiler revision reported
  in the driver's compiler flags.
- **Pipeline creation** took about 59 ms per pipeline on the console without a cache (112 pipelines in
  6.6 s). Persist a `VkPipelineCache` and, on top of it, a list of the pipelines the game uses so they
  can be created on a background thread before they are needed. See
  [native-renderer.md](native-renderer.md).
- **ZCULL** (hierarchical Z) is compiled out of the Switch build of NVK in the upstream port. See
  [mesa.md](mesa.md) for what it takes to enable it and what it gave.

## Presentation

Four limits of the Horizon WSI and of NVK on Maxwell that cannot be worked around:

1. **One Vulkan queue for the whole device.** Transfer queues require Turing. A presentation thread
   therefore shares the queue with rendering and cannot overlap with it.
2. **Swapchains have exactly three images.** Asking for four is silently clamped to three.
3. **Only one image can be acquired at a time.** A second acquire returns `VK_NOT_READY` or
   `VK_TIMEOUT`.
4. **Only FIFO and IMMEDIATE exist.** MAILBOX and FIFO_RELAXED silently fall back to FIFO. IMMEDIATE
   is `nwindowSetSwapInterval(nw, 0)` and **does not tear**: nvnflinger keeps composing at 60 Hz, it
   only stops blocking the producer.

The compositor quantises what is shown to 16.67 ms steps. Frame pacing on this platform is covered in
[measuring.md](measuring.md#frame-pacing-the-mean-is-not-what-the-player-feels).
