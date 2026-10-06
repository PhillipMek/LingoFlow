# Performance and Profiling

Date: 2026-10-06. Machine: Intel Core i7-8700 @ 3.20 GHz, 16 GB RAM, Windows 11,
Release build of the app and of the test binary unless stated otherwise (Debug
numbers are also given - Debug is the pessimistic budget). Scope per the task:
callback duration, CPU, RAM, network/UI load, callback headroom. "Optimize
measured bottlenecks": nothing measured qualified as a bottleneck, so the honest
outcome is "no code changes" (the task's own PASS criterion forbids unnecessary
architecture changes).

## 1. The callback budget (measured, pinned in CI)

`tests/performance/TestCallbackPerf.cpp` (tags `[perf]`, now part of ctest) times
thousands of real `AudioEngine::processAudio` calls on the production entry points
and geometries, and asserts headroom bounds derived from the measurements (10x+
margin: a regression ten times heavier trips them, machine noise cannot).

Release, 4000 iterations per path (Debug numbers in parentheses):

| Path | median us | p99 us | max us | 10 ms period used (p99) |
|---|---|---|---|---|
| mono 48k/480, underrun path ("dry night") | 8 (17) | 25 (36) | 93 (73) | 0.25% (0.36%) |
| mono 48k/480, forwarded + played (worker drain included) | 8 (17) | 34 (19-41) | 408 (41) | 0.34% |
| stereo 48k/480 | 17 (34) | 58 (39) | 165 (74) | 0.58% |
| 32 in / 32 out 48k/480 (SoundGrid ceiling, DSP is per-channel) | 281 (547) | 845 (1318) | 1546 (1442) | 8.5% |
| mono 88.2k/882 | 15 (31) | 46 (33) | 95 (57) | 0.46% |
| oversized block 8192 frames (~17x normal, the chunking path) | 143 (283) | 343 (439) | 447 (668) | 3.4% |

Committed headroom bounds (Debug-measured worst cases leave 10x-300x under them):
`< 500 us` mono dry, `< 1500 us` forwarded+played (the bound covers the test's ring
drain too - the worker's own cost), `< 1000 us` stereo, `< 4000 us` 32-channel,
`< 500 us` 88.2k, `< 8000 us` one oversized block (still under one period even
though the block carries 17 periods of audio).

**The headroom claim:** the worst real geometry (32x48k, Debug, p99) uses 13% of
the period; the product's actual MVP shape (mono in, one channel translated) uses
under 0.4%. The dominant cost per channel is the gain+meter pass over the block
(one multiply, one abs/compare chain per sample, straight-line SIMD-friendly
code); there is no lock, no allocation, no branch on the network side to make
cheaper. A venue can add a second translation feed before the callback itself
becomes the problem.

## 2. The worker-side capture path

Same file: `OpenAIRealtimeBackend::submitAudio` (validate + resample 48k->24k +
queue) timed from the streaming-worker thread against the scripted transport
(the sender's encode+send runs on its own thread at the append cadence, off the
audio path): median 0-2 us per 480-frame block, p99 6 us, max 119 us (Debug).
Against the worker's own 1-50 ms poll period this is noise; the bound asserts
p99 < 2 ms per block.

## 3. Live process behaviour (GUI + full dev chain, 5 minutes)

Release app, `--dev` (tone device thread driving real callbacks at 100 blocks/s,
mock translation at full frame flow, operator GUI open with its 500 ms UI poll),
10 s sampling:

| Metric | over 300 s |
|---|---|
| CPU (sum across cores, % of one core) | mean 2.13%, samples 1.1-3.3% |
| Private bytes | 57.9 -> 59.3 MB; +0.4 MB from sample 10 to 30, flattening - no progressive leak in the window |
| Working set | 52.1 -> 53.5 MB, same flattening |
| Threads | 43 -> 38 (JUCE pool spun down to steady state), stable |
| Handles | 571 -> 568, stable |
| Device pacing | 32 043 blocks delivered in 320.4 s (= exactly the 100 blocks/s the simulator promises), **0 late blocks** |
| Capture flow | 15 379 680 frames submitted = 320.4 s of room time, 0 gap-refused |
| Shutdown | graceful (WM_CLOSE), exit 0, all threads joined |

The CPU number includes the GUI's paint/timer work (the window was open the whole
run); the audio callback's own share is the microseconds in section 1.

**Boundary, stated instead of faked:** this is the developer chain on this machine.
Real SoundGrid driver behaviour under a real 8 h show, real-provider network cost,
and the venue's own background load are exactly what the long-run venue
checkpoint exists to measure - the protocol there is: run the real chain, export
diagnostics at show start and end, compare these same seven rows (blocks/late,
submitted/gap, underruns, reconnects, RAM). No number here is claimed for hardware
that was not present.

## 4. Verdict

- Measured: callback budget per path and geometry (pinned, CI-enforced with
  headroom bounds), worker capture path, live CPU/RAM/threads/handles/pacing.
- Safe headroom: the MVP callback uses <0.5% of its period (worst percentile,
  Release); even the 32-channel ceiling stays at 8.5% - the 10 ms period never
  comes close to exhaustion on this machine, Debug-inclusive.
- No bottleneck was measured that justified a code change; per the task's own PASS
  criterion ("no unnecessary architecture changes") none were made.
- Regression protection: the `[perf]` bounds now run inside ctest (359 total), so
  "unsafe callback budget or regression" becomes a build failure, not a venue
  surprise.

Commands: see docs/CI.md (`ctest -C Debug/-C Release`, `[perf] -s`,
perf sampler script retained in the temp tree).
