# Measuring performance without fooling yourself

This page explains how to measure performance on the Switch in a way you can trust. Read it before you optimise
anything, and again whenever a result surprises you. None of it is specific to Need for Speed.

Most of the time lost during this port was not spent writing code, but acting on wrong conclusions. Around 190 builds
were measured on a Switch at stock clocks (its normal speeds, without overclocking). A good share of the early
analysis had to be thrown away, because a number did not mean what it seemed to mean. This page collects the methods
that held up and the mistakes to avoid.

## In short

- Measure on the console, at stock clocks. Numbers from the PC or from an overclocked console mislead.
- To compare two versions, alternate between them every few seconds inside the same session, and check the noise
  with a setting that does nothing.
- On the Switch, multiply the raw GPU timestamps of NVK by 1.627.
- Before you trust a counter, read the code that increments it.
- Look at the share of slow frames and at the range of [frame times](glossary.md#frame-time), not only at the
  average: the player feels the variation.

## Ground rules

- **Measure on the console, at stock clocks.** The PC build is useful to check that something works and to estimate
  CPU cost, but its numbers do not carry over. A draw (one drawing command sent to the GPU) that cost 1.9 µs of CPU on
  a desktop PC cost around 16 µs on the Switch, and the GPU bottleneck of the console did not exist on the PC.
  Overclocking hides the real bottlenecks: a build that ran at 34 FPS with an overclock ran at 15 without one, and the
  part that limited it changed from the CPU to the GPU.
- **Report every number with its machine and its log.** Numbers used in one calculation must come from the same log.
  An average from one session multiplied by a counter from another session gives a number that means nothing.
- **Divide by what really causes the cost.** A stage that is paid per index (each index says which vertex to use
  next) looks eight times more expensive "per draw" in one part of a race than in another, without any code change.
  Use nanoseconds per index, per triangle or per fragment (a pixel being shaded): whatever drives the work.
- **Only compare the same kind of load.** Menus and loading screens cost half of what driving costs. Averaging a
  whole session inflated the frame rate by almost two frames per second. Use only the intervals of the race where
  the car is moving, and weight them by frames. When two sessions ran over different parts of the map, re-weight them
  by load (for example, by groups of draws per frame).

## A/B tests that can be trusted

An A/B test compares two versions of the same thing: A and B. Two runs of a quick race are not comparable: the
track, the car and the AI change every time. Two captures taken at the same second of two runs differ in up to 92 %
of their pixels, without any code change. Removing the AI does not help either, because the physics and the attract
demo (the demo the game plays by itself) still diverge.

What worked:

1. **Alternate A and B every N seconds inside the same session.** Put the change under test behind a setting that
   can be changed while the game runs, and change it every 30 s for timing or every 2 s for image checks. Write the
   current value of the setting to the log at every change, so the analysis can sort every report by mode.
2. **Pause the race** for GPU timing and for checking that two images are the same. The scene freezes, half of the
   screen stops changing, and the noise drops enough to see differences of a few percent. Compare only the pixels
   that do not change within each mode.
3. **Measure the noise with a control that cannot change anything.** Run the same alternation with a setting that
   has no effect. In one test, the "no effect" build showed a 9.9 % median difference, because the start of the race
   fell into one of the modes. Skip the start and compare interval by interval.
4. **Change one thing at a time.** When three optimisations alternate at once, the slow changes of the session leak
   into categories that none of them touch.
5. **Never run two measured processes at once.** Two overlapping runs made everything look 60 % slower.

## GPU timestamps on NVK under Horizon

A GPU timestamp is a reading of the GPU's clock, written between two commands. The difference between two of them
tells how long the GPU work in between took.

- **Multiply by 1.627.** [NVK](glossary.md#nvk) reports `timestampPeriod = 1 ns`, but on the Switch one timestamp
  unit is **1.627 ns**. This was measured by comparing the time the timestamps covered with real (wall clock) time.
  Every raw GPU time has to be multiplied by 1.627. A frame that looked like 14.7 ms of GPU work was really 23.9 ms,
  and a whole theory about the CPU and the GPU waiting for each other came from reading the raw value.
- **Check every GPU measurement against the wall clock.** The sum of the categories plus the idle gap, times the
  scale, must equal the frame time. If it does not, the scale or the categories are wrong. But once the scale is
  calibrated from wall time, this check is true by construction and stops proving anything.
- **`TOP_OF_PIPE` blurs the boundaries between passes.** `TOP_OF_PIPE` and `BOTTOM_OF_PIPE` are two of the points of
  the GPU's work where a timestamp can be written. On NVK, `TOP_OF_PIPE` is written when the GPU reads the command,
  not when earlier work finishes. So times per pass taken with it give part of each pass to the next one.
  `BOTTOM_OF_PIPE` gives exact boundaries. With `BOTTOM_OF_PIPE` on both sides, a counter of "overlapping jobs" can
  never be above zero, which leads to the next section.

## Counters that lie

1. **A wall-clock counter is not a cost.** "The ring thread is busy 97 % of the time" was really 76 % of CPU time
   plus 21 points of waiting. (The [ring thread](glossary.md#ring-thread) is the renderer thread that reads the
   game's GPU commands.) Always compare a thread's wall-clock time with its CPU time from a sampling profiler (a tool
   that looks many times per second at what each thread is doing). If they differ, the difference is waiting or
   preemption (the thread was paused so that another one could run), not work.
2. **One interval is not a series.** Quote the median of all intervals of the log, never one value that happens to
   support the argument.
3. **A "cost" that shrinks when frames get longer is slack.** A presentation stage that gets shorter when frames get
   longer measures the margin left before the vblank (the moment the screen starts its next refresh). It is spare
   time, not work.
4. **Check whether a counter is cumulative or per interval** before comparing it with another value. Cumulative
   means a total or average since the start. A cumulative average compared with a changing value gave a correlation
   of r = −0.22; done correctly it was +0.617.
5. **Counters with a limit hide data.** A stutter log limited to 200 entries filled up after 135 s of a 234 s
   session. So the second half of every race was invisible, and totals could not be compared.
6. **A [guard](glossary.md#guard) that counts zero because its code never ran proves nothing.** A safety counter read
   "0 conflicts" for five builds, because the feature was switched off in the settings file, so there was nothing to
   count. The first session where the code really ran showed one conflict per frame. Put an "attempts" counter next
   to every "failures" counter.
7. **A counter that cannot take any other value is not data.** If a counter reads the same value in every report,
   read the condition that increments it and ask whether it can ever be false.
8. **Comments that explain counters can be wrong.** A comment said a stage was divided by the draws that enter the
   ring. The increment was right after `vkCmdDraw`, so it counted recorded draws only, and every stage was inflated by
   40 %. Check against the line that increments the counter, and against the arithmetic of the log itself.
9. **A counter measures what its code does, not what its label says.** A report flagged two render targets as
   "nobody reads this". The counter only tracked copies back to the game's memory, and both targets were read as
   textures later in the frame. Removing them would have broken the image. A warning in a log is a hypothesis, not a
   conclusion.
10. **Count real actions, not calls.** A [hook](glossary.md#hook) ran on every call, but an unverified offset made
    it skip almost all of them. It looked active in the log and changed nothing. Pair "times called" with "times it
    acted".
11. **Stopwatches cost time.** Timing every ring packet and every draw stage meant about 50,000 clock reads per frame:
    close to a quarter of the ring thread's CPU on the PC. On busy code, time only a sample (today one in 128 packets,
    and one in 8 or 64 draws depending on the timer) and scale the result.

## Verifying a change

- **Checking that a change was applied is not checking that it worked.** One build made NVK's command buffer memory
  go through the CPU cache, because of a neat argument about memory latency. The change was applied: cache
  maintenance traffic grew by exactly the predicted amount per draw. But the cost of recording did not move (the
  difference was 0.78 σ, which is noise). The reason: on the Switch's CPU, a Cortex-A57, writes to memory without
  cache are collected and merged by a write buffer. The latency in the argument belonged to reads and to device
  memory. If a theory predicts a large effect, give it a cheap test before spending a build on it.
- **Asynchronous system calls need a delayed check.** `apmSetPerformanceConfiguration` returns success before the
  clocks change, and later `pcv` puts back the memory clock of the active configuration (details in
  [platform-notes.md](platform-notes.md#clocks)). Check after a delay, keep a periodic check, and read the real table
  of configuration IDs instead of guessing neighbouring IDs by arithmetic.
- **Do not discard an idea with a measure that does not measure it.** A proposal to pin the ring thread to one core
  was first rejected by pointing at the load of each core of the whole system, which says nothing about threads
  moving between cores. Measured properly: the "preferred core" setting does not pin anything in Horizon (0.24 moves
  per ring loop wherever it is set), moves cost about 0.05 % of CPU, and an exclusive core mask does pin the thread
  but makes the worst frames worse (minimum 16.3 FPS against 21). The idea was closed, but with the right numbers.
- **When a measured section costs ten times what its code can cost, it is a bug.** For six builds, a recording stage
  showed 12-53 ms for about twenty Vulkan calls. Splitting it into four timers found the cause in one test: the
  swapchain was being recreated every frame (see [performance-history.md](performance-history.md)).

## Frame pacing: the mean is not what the player feels

*Frame pacing* is how evenly the frames reach the screen. The Switch compositor (the system part that puts the final
image on screen) runs at 60 Hz with FIFO presentation (see [platform-notes.md](platform-notes.md#presentation)), so
every frame is shown for a whole number of vblanks. Frame times are rounded up to steps of 16.67 ms (33.3, 50.0,
66.7 ms...). A game that averages 38 ms alternates between two and three vblanks.

- **A regular 38 ms frame feels better than one that jumps between 33 and 50.** One build gained 1.5 FPS on average
  with a presentation thread of its own, but made the range of frame times three times wider and more than doubled
  the frames above 50 ms. Players noticed the regression every time, before any measurement did. Always report the
  share of frames above 50 ms and the range, not only the average.
- **Reaching 33.3 ms on average does not remove stutter.** A simulation over 30,467 race frames (each frame shown for
  `ceil(T[i+1] / 16.67) − ceil(T[i] / 16.67)` vblanks) gave these deviations of the frame time the player sees:

  | Mean frame time | Deviation of what is shown | Frames that change cadence |
  |---|---|---|
  | 41.3 ms | 10.75 ms | 63.6 % |
  | 35.0 ms | 9.70 ms | 53.7 % |
  | 33.3 ms | 9.44 ms | 50.9 % |
  | 30.0 ms | 9.04 ms | 53.2 % |

  When the game makes frames as fast as it can (without waiting for the screen), and frame times vary by around 20 %
  from one frame to the next, no mean frame time avoids falling between two vblank steps. Two things together bring
  the deviation to zero:
  - frames that fit in about 31 ms (a median around 28 ms, so that 90 % of frames fit), **and**
  - a swap interval of 2, which shows each frame on every second vblank only.

  Either one alone leaves about 9 ms of deviation. A swap interval of 2 before frames fit makes things worse, because
  a late frame then costs 33.3 ms instead of 16.7.
- **More swapchain images do not help.** Adding swapchain images does not change the steps; it only shifts when the
  frames land.
- **Counting frames from a video can be wrong.** Video-based frame counting (for example with `mpdecimate`, an FFmpeg
  filter that drops repeated frames) undercounts in repetitive scenery, such as a street lined with identical fences.
  It reports pauses that the logs contradict.
