# Latency budget (task 018)

Dated 2026-10-04. This file is where latency numbers may live once measured;
**code never holds measured latency** (AGENTS.md 19), and this document holds
none today either - every numeric here is definition or arithmetic, labeled as
such, and the measurement fields are deliberately blank.

## The component model

Sound travels this path; each named component is what `latencyAccounting()`
(`src/App/UiModel.cpp`) renders on screen and into the diagnostics export:

```text
speaker/room -> [A] ASIO input -> [B] capture block -> [C] translator path
             -> [D] jitter queue -> [E] playback block -> [F] ASIO output -> audience
```

| # | Component | What we can state today | Kind |
|---|---|---|---|
| A | ASIO input latency | whatever `getInputLatencyInSamples()` answers via `DeviceCapabilities`; ASIO cannot distinguish "zero" from "not reported", so a zero is rendered "not reported" and contributes 0.0 to the visible total | driver-reported |
| B | capture block | `bufferFrames / sampleRate` - at the 48 kHz/480-frame defaults that is 10 ms **by arithmetic, not measurement** | arithmetic |
| C | network + model | **one combined row, never split**: the live backlog = frames submitted (012 counter) minus frames that came back in any form (accepted + rejected + dropped), at device rate. This is a computed queue depth, not an RTT and not server time; splitting wire from model would need a provider-side timestamp that the API does not offer, and inventing one is exactly what AGENTS.md 19 forbids | live-computed |
| D | jitter queue | live fill (atomic) plus the configured pre-roll target | live / configuration |
| E | playback block | same arithmetic as B | arithmetic |
| F | ASIO output latency | as A | driver-reported |
| - | estimated total | the sum of the rows above with unreported drivers counting visibly as 0.0; **it is an accounting sum, not mouth-to-ear**, and every renderer of it says so | labeled sum |

Two pipeline stages are deliberately *not* separate rows: the ring-buffer and
streamer transit (sub-millisecond queueing bounded by block size, folded into
B/C's arithmetic), and the provider's own internal buffering, which is inside C.

## What the live backlog row is - and is not

While a sentence is being translated, C grows and drains as audio enters and
returns: it is an honest, continuously computed indicator of "how much audio is
in the path right now". It is not the answer to "how long does one sentence
take" (that needs a stopwatch: speak, watch it come back) and not RTT. If C
grows without draining, something is wrong (stalled network, starved playback),
which is a diagnostic reading, not a latency claim. The clamp-to-zero and the
thread caveat travel in the row's own kind text.

## Measurement fields - to be filled at the rig, dated, never backfilled from code

The venue run-sheet (`docs/rig-checklist.md` step 7/8) produces these. Fill the
observed value plus date, device and settings under it; do not average silently
across conditions, and do not move any of these numbers into code as constants.

| Fact | Observed | Date / device / settings |
|---|---|---|
| Mouth-to-ear, loopback (no translation): speak into input channel, hear the monitor | ____ | ____ |
| Mouth-to-ear, live translation EN->RU (012 check), first words audible | ____ | ____ |
| Session open (log: `connecting` -> `connected`) | ____ | ____ |
| Delta arrival vs realtime (protocol doc section 15 burst facts refined with the rig) | ____ | ____ |
| Jitter sweep (step 7) result for the pre-roll default | ____ | ____ |
| ASIO reported latencies at the venue (A/F rows, values from the export) | ____ | ____ |

The `diagnostics` export from the same press (017) already contains the [latency]
section - the receipt of what the software saw during the observation.

## Limitations, stated plainly

- A/F: a driver may answer 0 or nothing; we never render silence as speed.
- C: no provider timestamps exist in this API; wire-vs-model separation is
  beyond our reach, and any future claim otherwise would be fabricated.
- The total excludes anything unmeasured by definition - that is why the
  venue table above, not the sum, is what a sound engineer should quote.
- The pre-roll (D's configured target) is a *chosen* delay, currently the 250 ms
  provisional default pending the step-7 sweep; it is a policy, listed under
  the same rules as measurements in `docs/device-defaults.md`.
