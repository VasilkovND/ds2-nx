# Measuring performance without fooling yourself

Most of the time lost during this port was not spent writing code but acting on wrong conclusions.
Around 190 builds were measured on a Switch at stock clocks, and a good share of the early analysis
had to be thrown away because a number did not mean what it seemed to mean. This page collects the
methods that held up and the mistakes that did not. None of it is specific to Need for Speed.

## Ground rules

- **Measure on the console, at stock clocks.** The PC build is useful to check that something works
  and to estimate CPU cost, but its numbers do not transfer. A draw that cost 1.9 µs of CPU on a desktop
  cost around 16 µs on the Switch, and the GPU bottleneck on the console did not exist on the PC.
  Overclocking hides the real bottlenecks: a build that ran at 34 FPS with an overclock ran at 15
  without one, and the limiting unit changed from the CPU to the GPU.
- **Report every number with its machine and its log.** Numbers used in one calculation must come
  from the same log. Multiplying an average from one session by a counter from another session gives
  a number that means nothing.
- **Normalise by the unit that actually generates the cost.** A stage that is paid per index looks
  like it gets eight times more expensive "per draw" between two parts of the same race without any
  code change. Use nanoseconds per index, per triangle or per fragment, whatever drives the work.
- **Only compare the same kind of load.** Menus and loading screens cost half of what driving
  costs. Averaging a whole session inflated the frame rate by almost two frames per second. Use the
  in-motion race intervals only, weight them by frames, and when two sessions ran over different
  parts of the map, re-weight them by load (for example by draws per frame buckets).

## A/B tests that can be trusted

Two runs of a quick race are not comparable: the track, the car and the AI change every time, and
two captures taken at the same second of two runs differ in up to 92 % of their pixels without any
code change. Removing the AI does not help either, because physics and the attract demo still
diverge.

What worked:

1. Put the switch under test behind a runtime setting and **alternate it every N seconds inside the
   same session** (30 s for timing, 2 s for image checks). Write the current value of the setting in
   the log at every change, so the analysis can bin every report by mode.
2. For GPU timing and image equivalence, **pause the race**: the scene freezes, half of the screen
   stops changing and the noise drops enough to see differences of a few percent. Compare only the
   pixels that do not change within each mode.
3. **Calibrate the noise with a control that cannot change anything.** Run the same alternation with
   a setting that has no effect. In one test the "no effect" build showed a 9.9 % median difference,
   caused by the start of the race falling into one of the modes. Skip the start and compare
   interval by interval.
4. **Change one thing at a time.** Alternating three optimisations at once lets the drift of the
   session leak into categories that none of them touch.
5. **Never run two measured processes at once.** Two overlapping runs made everything look 60 %
   slower.

## GPU timestamps on NVK under Horizon

- NVK reports `timestampPeriod = 1 ns`, but on the Switch one timestamp unit is **1.627 ns**. The
  ratio was measured over 147 intervals, menus included, by comparing the covered timeline with wall
  clock time. Every raw GPU time has to be multiplied by 1.627. A frame that looked like 14.7 ms of
  GPU work was really 23.9 ms, and a whole hypothesis about CPU/GPU serialisation came from reading
  the raw value.
- **Check every GPU measurement against the wall clock.** The sum of the categories plus the idle
  gap, times the scale, must equal the frame time. If it does not, the scale or the categories are
  wrong. Be aware that once the scale is calibrated from wall time, this identity holds by
  construction and stops proving anything.
- On NVK, `TOP_OF_PIPE` is released when the command is read, not when earlier work finishes, so
  per-pass splits taken with it attribute part of each pass to the next one. `BOTTOM_OF_PIPE` gives
  exact boundaries. With `BOTTOM_OF_PIPE` on both sides, "overlapping jobs" can never be non-zero,
  which leads to the next section.

## Counters that lie

1. **A wall-clock counter is not a cost.** "The ring thread is busy 97 % of the time" was really 76 %
   of CPU time plus 21 points of waiting. Always compare a thread's wall-clock time with its CPU time
   from a sampling profiler. If they differ, the difference is waiting or preemption, not work.
2. **One interval is not a series.** Quote the median of all intervals, never one value that
   happens to support the argument.
3. **A "cost" that correlates negatively with frame time is slack.** A present stage that gets
   shorter when frames get longer measures the margin left before the vblank.
4. **Check whether a counter is cumulative or per interval** before correlating it. A cumulative
   average correlated against a fluctuating value gave r = −0.22; done correctly it was +0.617.
5. **Capped counters hide data.** A stutter log capped at 200 entries filled up after 135 s of a
   234 s session, so the second half of every race was invisible and totals could not be compared.
6. **A guard that counts zero because its code never ran proves nothing.** A safety counter read
   "0 conflicts" for five builds because the feature was switched off in the settings file, so there
   was nothing to count. The first session where the path really ran showed one conflict per frame.
   Put an "attempts" counter next to every "failures" counter.
7. **A counter that cannot take any other value is not data.** If a counter reads the same value in
   every report, read the condition that increments it and ask whether it can ever be false.
8. **Comments that explain counters can be wrong.** A comment claimed a stage was divided by the
   draws that enter the ring; the increment was right after `vkCmdDraw`, so it counted recorded draws
   only, and every stage was inflated by 40 %. Verify against the line that increments it and against
   the arithmetic of the log itself.
9. **A counter measures what its code does, not what its label says.** A report flagged two render
   targets as "nobody reads this". The counter only tracked read-backs to guest memory; both targets
   were sampled as textures later in the frame. Removing them would have broken the image. A warning
   in a log is a hypothesis, not a conclusion.
10. **Count real actions, not calls.** A hook that ran on every call but filtered almost all of them
    away through an unverified offset looked active in the log and changed nothing. Pair "times
    called" with "times it acted".
11. **Stopwatches cost time.** Timing every ring packet and every draw stage meant about 50,000 clock
    reads per frame, close to a quarter of the ring thread's CPU on the PC. Sample hot paths (one in
    16 packets, one in 8 or 64 draws) and scale the result.

## Verifying a change

- **Checking that a change was applied is not checking that it worked.** One build made the NVK
  command buffer memory cacheable on the CPU, based on a neat latency argument. The change was applied
  (cache maintenance traffic grew by exactly the predicted amount per draw) and the recording cost
  did not move (0.78 σ). Posted stores to non-cacheable normal memory are merged by the write buffer
  of the Cortex-A57; the latency in the argument belonged to reads and device memory. If a theory
  predicts a large effect, give it a cheap test before spending a build on it.
- **Asynchronous system calls need a delayed check.** `apmSetPerformanceConfiguration` returns
  success before the clocks change; reading the clock on the next line shows the old value. Later,
  `pcv` reimposes the memory clock of the active configuration, so direct changes through `clkrst`
  are accepted and then reverted. Verify after a delay, keep a periodic check, and read the real table
  of configuration IDs instead of guessing neighbours by arithmetic.
- **Do not discard an idea with a measure that does not measure it.** A proposal to pin the ring
  thread to a core was first rejected by pointing at the per-core load of the whole system, which says
  nothing about migrations. Measured properly, the "preferred core" setting does not pin anything in
  Horizon (0.24 migrations per ring loop wherever it is set), migrations cost about 0.05 % of CPU, and
  an exclusive affinity mask pins the thread and makes the worst frames worse (minimum 16.3 FPS
  against 21). The idea was closed, but with the right numbers.
- **When a measured section costs ten times what its code can cost, it is a bug.** A recording stage
  showed 12-53 ms for about twenty Vulkan calls for six builds. Splitting it into four timers found
  the cause in one test: the swapchain was being recreated every frame (see
  [performance-history.md](performance-history.md)).

## Frame pacing: the mean is not what the player feels

The Switch compositor runs at 60 Hz with FIFO presentation, so every frame is shown for a whole
number of vblanks. Frame times are quantised to 16.67 ms steps (33.3, 50.0, 66.7 ms...), and a game
that averages 38 ms alternates between two and three vblanks.

- **A regular 38 ms frame feels better than one that jumps between 33 and 50.** One build gained
  1.5 FPS on average with a dedicated presentation thread but widened the frame-time range three
  times and more than doubled the frames above 50 ms. Players noticed the regression every time,
  before any measurement did. Always report the share of frames above 50 ms and the range, not just
  the average.
- **Reaching 33.3 ms on average does not remove stutter.** A simulation over 30,467 race frames
  (each frame shown for `ceil(T[i+1] / 16.67) − ceil(T[i] / 16.67)` vblanks) gave these deviations of
  the displayed frame time:

  | Mean frame time | Deviation of what is shown | Frames that change cadence |
  |---|---|---|
  | 41.3 ms | 10.75 ms | 63.6 % |
  | 35.0 ms | 9.70 ms | 53.7 % |
  | 33.3 ms | 9.44 ms | 50.9 % |
  | 30.0 ms | 9.04 ms | 53.2 % |

  With a free-running producer and around 20 % frame-to-frame variation, there is no mean frame time
  that avoids straddling two vblank buckets. What brings the deviation to zero is the combination of
  frames that fit in about 31 ms (median around 28 ms to cover the 90th percentile) **and** a swap
  interval of 2, which pins presentation to every second vblank. Either one alone leaves about 9 ms
  of deviation. A swap interval of 2 before frames fit makes things worse, because a late frame then
  costs 33.3 ms instead of 16.7.
- Adding swapchain images does not change the quantisation; it only shifts the phase.
- Video-based frame counting (for example `mpdecimate`) undercounts in repetitive scenery, such as a
  street lined with identical fences, and reports pauses that the logs contradict.
