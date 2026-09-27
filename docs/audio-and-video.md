# Audio and video on the Switch

Audio problems showed up before frame rate problems and took the longest to close. Everything here
concerns the ReXGlue audio path (XAudio, XMA decoding and the Switch output driver) and the game's
video player, so most of it applies to other ports too.

## Output driver

The Switch output driver in the SDK (`switch_audio_system.cpp`) opens audout at 48 kHz, stereo,
16-bit, with three 1,024-sample buffers. Two things were needed on top of the stock behaviour:

- **A pump at 187.5 Hz.** Without it, the driver asked the game for four frames in a row and then
  nothing, which starved the game's voice between bursts.
- **A small queue of mixed frames** (10 frames, 53 ms). It absorbs short delays of the game's audio
  thread without adding audible latency.

The downmix to stereo uses 0.586 for front, 0.414 for centre and rear, no LFE, with hard clipping.
Measured over HDMI captures, the mix peaks at 0.93-0.95 and never clips.

## "Robotic" audio

Symptom: a metallic, stuttering sound everywhere, worse in crashes and busy areas, independent of
the frame rate.

Mechanism: the game's audio server thread keeps a ring of two 256-sample packets (10.7 ms) for its
XAudio voice, and each mixed frame consumes one. When the server is late, the frame goes out with
that voice silent: 5.33 ms gaps interleaved with sound. No audio is lost, silence is inserted.

It took three separate fixes, in this order of weight:

1. **Priority.** The server was a guest thread at 0x3B, sharing 10 ms time slices with the main game
   thread and another busy guest thread, while the GPU ring ran above it. In crashes it delivered 89 %
   of its packets. Raising it to 0x2D, and letting the audio worker wait up to 30 ms for a late packet,
   made it "barely noticeable". The SDK's priority table also had a bug: it compared thread names
   exactly, but thread names get a ` (F80000xx)` suffix, so the audio worker and the XMA decoder never
   got their intended priority. Compare with `starts_with`.
2. **Native versions of the hottest audio functions.** Two resamplers, a filter, a gain sum and
   `memset` were rewritten in C++ that performs the same floating-point operations in the same order
   (`double(float(std::fma(...)))`, and `fnmsubs` as `-std::fma(x, y, -z)`), with zero differences
   over 159 million samples. The server's CPU time dropped by 17.5 %.
3. **XMA decoding off the game thread.** `XMAEnableContext` writes the kick register, and the SDK
   decoded the context right there, on the game's audio thread. On the Switch that cost 17.3 % of a
   core at 1,555 kicks per second, with peaks of 14.4 ms against a 10.7 ms buffer, and the GPU ring
   thread preempted the decode midway. Nothing in the game calls `WaitForWorkDone()`: the game polls
   the context. So the kick now only marks the context in a bitmap and wakes the XMA worker thread;
   the full sweep stays as a safety net. Kick-to-data latency averaged 93 µs on the console (worst
   8.3 ms, which did not produce a gap), the audio thread went from 17.3 % to 1.5 % of a core, gaps
   in races went from 43 to 5, and the race frame rate went up by 3.5 FPS as a side effect.

Tools that made this measurable: counters of empty mixing passes and delivered packets per 500 ms,
kick-to-data latency, a WAV dump of the game's six-channel PCM before the driver, per-context XMA
dumps, and a gap detector run over the audio track of console video captures.

## An intermittent hang of the audio server

In about three of seven early runs, the game went completely silent and the race never finished
loading. The audio server thread (`sub_825E3E28` in this game) waits for its event without a timeout;
when it wakes up with its slot still busy, it goes back to waiting without trying to deliver, so a
single lost end-of-packet notification stops it for good, and with it the game's audio command queue.

The fix adds a timeout to that wait and, after 250 ms without progress, retries the delivery and frees
the slot. A timeout alone is not enough: the loop must retry. Lesson: one run with working audio does
not prove a build is good when the failure is intermittent.

## Videos

The game's WMV3 decoder, recompiled from the Xbox 360 libraries, took 98 % of a core on the Switch
and produced about 22 frames per second of 720p30 video, so videos stuttered and their audio ended
before the picture. The port feeds the same bytes to FFmpeg's WMV3 decoder instead, through the
game's own data callbacks, and writes the decoded planes where the game expects them. Videos now take
7-15 ms per frame.

- FFmpeg is linked as LGPL 2.1 (the SDK's build, configured without GPL components). Do not link a
  GPL build.
- Traps: identify the video by the stream being decoded, not by the last `.wmv` opened (the game opens
  the next one early); read through the game's data callback instead of opening the file separately;
  leave the "data remaining" count where the game expects it, or it drops the next frame.
- A shadow mode decodes with both decoders and compares; the videos were identical bit for bit except
  for three short sections of one video with a mean difference of 0.02.
- During videos the game presents 30 frames per second, so frame counters read 30, not 60.

## Codec reinitialisation cost

XMA contexts reopen the FFmpeg decoder for every new sound. `ff_mdct_init`, the split-radix
permutation and the sine/cosine tables took about 28 % of the XMA thread in busy moments. Caching
the bit-reversal table and the twiddle factors in FFmpeg's FFT and MDCT templates removed that cost.
