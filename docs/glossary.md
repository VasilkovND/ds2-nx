# Glossary

Every technical word used in these documents, explained in plain words. If a document uses a term you do not know,
look for it here. The terms are grouped: the Xbox 360, the recompilation, the Switch, graphics, building, and this port.

## The Xbox 360

### Guest

The original game and everything that belongs to its world: its code, its memory, its threads. "Guest memory" is the
memory as the game sees it. The opposite is the [host](#host).

### Host

The machine that really runs the port: the Switch, or a PC while developing. Code written for the port itself is
"host code" or "native code".

### XEX

The Xbox 360's executable format. The game's program is `default.xex`, on the disc. Each [edition](#edition) of the
game has its own `default.xex`.

### PowerPC

The kind of processor the Xbox 360 has. The game's code is PowerPC code, and the Switch's ARM processor cannot run it.
That is why the code is [recompiled](#static-recompilation).

### Xenos

The Xbox 360's graphics chip (its GPU).

### EDRAM

A small, very fast memory (10 MB) inside the [Xenos](#xenos), where the Xbox 360 draws. It is so small that the game
draws big images in pieces ([tiling](#tiling)) and then copies the result to normal memory ([resolve](#resolve)).
The Switch has nothing like it.

### Tiling

Two meanings, both from the Xbox 360:

- Drawing a frame in several horizontal strips, because the whole image does not fit in the [EDRAM](#edram). The
  Switch does not need it, so the port draws each frame in one go.
- Textures stored in a scrambled order that the Xenos reads faster ("tiled textures"). The port puts them back in
  normal order when it copies them to the Switch GPU ("untiling").

### Resolve

Copying a finished image out of the [EDRAM](#edram) to normal memory, so it can be shown on screen or used as a
texture. The game does many resolves in each frame.

### PM4 ring

The list of commands the game writes for the GPU: draw this, change that setting, copy this image. On the Xbox 360
the GPU reads it. In this port, the [ring thread](#ring-thread) reads it and turns it into [Vulkan](#vulkan). "PM4" is
the name of the command format, and "ring" means that the list is a circular buffer: when it reaches the end, it
starts again from the beginning.

### Direct3D

The graphics library the game uses to talk to the GPU. On the Xbox 360 it is compiled into the game itself
(`default.xex`), so it is recompiled along with the game. It is the part of the game that writes the
[PM4 ring](#pm4-ring).

### Microcode

The game's [shaders](#shader) in the Xbox 360 GPU's own format. They are stored in the game files, and the port
translates them with [XenosRecomp](#xenosrecomp).

### XMA

The Xbox 360's compressed audio format. The Switch cannot play it directly, so the port decodes it with FFmpeg.

### WMV3

The video format of the game's cutscenes (Windows Media Video 9). The port decodes it with FFmpeg.

## The recompilation

### Static recompilation

Translating a whole program to another processor before it runs. An emulator does it while the game runs, which is
much slower. Here, every function of `default.xex` is translated from [PowerPC](#powerpc) to C++, and that C++ is
compiled for the Switch.

### ReXGlue

The recompiler and support library this port is built on ([rexglue-sdk](https://github.com/rexglue/rexglue-sdk)).
It does two jobs:

- it translates the game to C++ (this step is called code generation, "codegen");
- it replaces the Xbox 360 system the game expects: its kernel, file system, audio, controllers and graphics.

It is based on the work of the Xenia emulator. This port's version is in `sdk/`, with a new layer for the Switch.

### Hook

A place where the port puts its own code in place of one of the game's functions, or around it. The game still calls
the function as always, but the port's code runs.

### Native replacement

A game function rewritten by hand, so that it runs faster than its recompiled version. Only the functions that take
the most CPU time get one. Each comes with a [guard](#guard).

### Guard

A safety check that comes with every [native replacement](#native-replacement). For a while, or on some calls, the
port runs both the original function and the replacement and compares their results. If they ever differ, it writes
`DIFERENCIA` in the log and turns the replacement off until the game is closed.

## The Switch

### Horizon

The Switch's operating system.

### libnx

The library that homebrew uses to talk to [Horizon](#horizon): threads, files, controllers, screen, audio. It is part
of [devkitPro](#devkitpro).

### devkitPro

The free set of tools for making Switch homebrew ([devkitpro.org](https://devkitpro.org)). It includes devkitA64, the
compiler for the Switch's processor, and [libnx](#libnx).

### Homebrew

Unofficial software for the Switch. It needs a console with custom firmware (Atmosphère).

### NRO

The file format of Switch homebrew programs. The port is `nfsmw-nx.nro`. Inside it are the program, its icon, and its
name and version (the "NACP").

### Title takeover

A way to start homebrew with all the memory of a game: hold **R** while you start any installed game, and the Homebrew
Menu opens instead. The port needs it, or a [forwarder](#forwarder), because it uses a lot of memory.

### Forwarder

A small installed title that starts a homebrew [NRO](#nro) as if it were a game, with the memory of a game. The README
explains how to make one with Sphaira.

### Tegra X1

The chip inside the Switch: an ARM processor with 4 cores, 3 of them for the game, and an NVIDIA GPU of the Maxwell
generation (called GM20B). "Erista" is the original 2017 chip and "Mariko" the newer one (Switch V2, Lite and OLED).

## Graphics

### Vulkan

A modern graphics API: a standard way for programs to talk to the GPU. The port draws everything with Vulkan.

### Mesa

A big open source project that contains many graphics drivers. [NVK](#nvk) and [NAK](#nak) are part of it. This port
uses a version of Mesa that runs on the Switch (mesa-switch). See [mesa.md](mesa.md).

### NVK

Mesa's [Vulkan](#vulkan) driver for NVIDIA GPUs. On the Switch it turns the port's Vulkan calls into commands for the
GPU. It is included inside the NRO.

### NAK

The shader compiler of [NVK](#nvk). It turns [SPIR-V](#spir-v) shaders into the Switch GPU's own machine code when a
[pipeline](#pipeline) is created.

### Shader

A small program that runs on the GPU, for example to decide the color of each pixel. The game has hundreds.

### XenosRecomp

The tool that translates the game's shader [microcode](#microcode) into [HLSL](#hlsl)
([hedge-dev/XenosRecomp](https://github.com/hedge-dev/XenosRecomp)). This port's version, with its fixes, is in
`shaders/`.

### HLSL

A shader language that people can read and write, the one of Direct3D on PC. [XenosRecomp](#xenosrecomp) writes the
game's shaders in it.

### DXC

Microsoft's shader compiler. The port uses it to turn the [HLSL](#hlsl) shaders into [SPIR-V](#spir-v).

### SPIR-V

The shader format that [Vulkan](#vulkan) accepts.

### Shader library

The file `nfsmw_shaders.nfsp`, with all of the game's shaders already translated to [SPIR-V](#spir-v). The installer
page makes it from the user's own disc, because the shaders are part of the game and cannot be shared.

### Render target

An image the GPU draws into: the screen, the shadow map, a reflection. On the Xbox 360 they live in the
[EDRAM](#edram); in this port they are normal Vulkan images.

### Pipeline

In [Vulkan](#vulkan), a complete set of drawing settings together with its compiled [shaders](#shader). Creating one
on the Switch takes between about 60 and 160 milliseconds, because [NAK](#nak) compiles the shaders at that moment.
If that happens while playing, it causes a stutter.

### Pipeline cache

A file where the compiled [pipelines](#pipeline) are saved, so they are not compiled again next time:
`cache/nfsmw_nativo_pipelines.bin` in the port's folder. That is why the first race after installing can stutter,
and later ones do not.

### ZCULL

A feature of NVIDIA GPUs that throws away hidden pixels early, before their [shader](#shader) runs. The Switch driver
did not use it. This port turned it on in its version of [NVK](#nvk), and it saved a lot of GPU time.

### Ring thread

The port's thread that reads the game's [PM4 ring](#pm4-ring) and records the [Vulkan](#vulkan) commands. It is one
of the busiest threads, so much of the optimization work went into it.

### Frame time

How long one frame takes, in milliseconds. 33.3 ms is 30 frames per second, 16.7 ms is 60.

### FPS

Frames per second: how many images the game shows each second.

## Building

### LTO

Link time optimization. The compiler optimizes the whole program at once, at the end of the build, instead of one
file at a time. The NRO gets faster and the build slower.

### PGO

Profile guided optimization. First, a special build records which code runs most while someone plays (this record is
the "profile"). Then the normal build uses it to optimize and arrange that code better. The profiles are in `pgo/`.

### Function ordering

Putting the functions that run most often next to each other in the program, so the processor keeps them in its fast
memory (its cache) more easily. The list is `app/orden_funciones.ld`.

## This port

### cvar

A setting of the program ("console variable"). Each one has a name, a default value written in the code and a
description. You can change it in [nfsmw.toml](#nfsmwtoml) or in the [Debug Menu](#debug-menu). This port's own
settings start with `nfsmw_` and have Spanish names.

### nfsmw.toml

The settings file next to the NRO. Each line sets one [cvar](#cvar): `name = value`. If you delete a line, that
setting goes back to its default value from the code.

### Debug Menu

The settings menu the port shows over the game when you press **L + R + Right**. It lists every [cvar](#cvar) by
category. The main README explains it.

### Edition

A version of the game for one region or language: PAL Spanish, NTSC-U, and so on. Each edition has a different
`default.xex`, so each one needs its own NRO. See [editions.md](editions.md).
