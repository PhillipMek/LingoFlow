# Audio device defaults without a SoundGrid server

Decided 2026-10-01 by the product owner: **this development PC will never have a
SoundGrid server on it.** The Waves SoundGrid ASIO driver is installed, the server
is not reachable, so real-device behaviour cannot be observed here at any point in
development.

This document records what that means for defaults, what is verified anyway, and
what stays deferred to a machine that does have a server.

## What is verified on this machine (no server needed)

| Fact | Evidence |
|---|---|
| Driver binaries installed | `C:\Windows\System32\SoundGridAsio.dll` (x64) and `SysWOW64\SoundGridAsio.dll` (x86), FileDescription "Waves SoundGrid ASIO Driver" 16.5.197.301 |
| Kernel stack present and running | `SoundGridProtocol.sys` (Auto, Running), `SoundGridMidi.sys` (Running) |
| COM class registered | `HKLM\SOFTWARE\Classes\CLSID\{75BB3792-DCE7-4199-9935-4C1BB8F50C90}\InprocServer32` = `c:\windows\system32\soundgridasio.dll`, `WOW6432Node` twin present |
| Host-visible ASIO registration | `HKLM\SOFTWARE\ASIO\Waves SoundGrid` with `CLSID` and `Description`, plus `HKLM\SOFTWARE\WOW6432Node\ASIO\Waves SoundGrid` |
| This is the path JUCE reads | `ASIOAudioIODeviceType::scanForDevices()` opens `HKEY_LOCAL_MACHINE\software\asio`, reads `clsid`, and `checkClassIsOk()` requires `HKCR\CLSID\<id>\InprocServer32` |
| Driver entry points | `dumpbin /exports` shows `DllGetClassObject`, `DllCanUnloadNow`, `DllRegisterServer`, `DllUnregisterServer` - the COM-style loading path JUCE uses |
| Server has never been configured here | `HKCU\SOFTWARE\Waves\SoundGrid\ASIO Driver\Configuration` exists and is **empty** |

Consequence: enumeration of "Waves SoundGrid" in our own ASIO discovery code is
agent-verifiable. Opening it is not.

## What is measured on this machine (2026-10-01, task 004)

The probe tool (`lingoflow_asio_probe`) was run against the installed Waves driver.
**The driver opens and runs even with no SoundGrid server present**, so most of the
"needs hardware" list turned out to be verifiable here after all. Measured, Debug and
Release builds, 10 open/start/stop/close cycles:

| Property | Measured value |
|---|---|
| ASIO devices enumerated | 1: `Waves SoundGrid ASIO` |
| Registry key vs reported name | key `Waves SoundGrid`, driver-reported name `Waves SoundGrid ASIO` (matching is therefore by containment, checked by `--verify`) |
| `open()` without a server | succeeds |
| Channels reported | 32 input, 32 output |
| Channel names reported | `SoundGrid 1` … `SoundGrid 32` in both directions - generic driver names, not the console patch names |
| Sample rates reported | `44100, 48000, 88200, 96000` |
| Buffer sizes reported | **one size: 256 frames**; the requested 480 is not offered |
| Active configuration after open | 48000 Hz, 256 frames, 1 in + 1 out channel |
| Driver-reported latency | input 384 samples (8 ms), output 256 samples (5.3 ms) |
| Bit depth reported | 32 |
| Callback delivery while running | ~219 blocks per 1.2 s at 256/48000 (expected 225), the engine processed the same number of blocks (within one - the two counters are read at different instants) |
| xrun counter | **not reported by this driver** (JUCE returns -1; shown as text, never as "0") |
| State after `deactivate()` | `closed` in every cycle, no crash, no hang, no leak symptom |
| Control panel | exposed by the driver (`hasControlPanel`) |

Two consequences recorded in code:
* `asio::describeXRunCount` renders a negative counter as "not reported by this
  driver". Task 005 must therefore compute its own underrun/overrun counters from
  callback timing instead of trusting the driver.
* `audio::DeviceRequest` carries one-based channel indices, and
  `asio::selectConfiguration`/`asio::validateChannelSelection` keep the fallback to a
  offered rate/buffer **logged**, never silent.

Also found and fixed while measuring: the config validation window for gain was
`-60..+12 dB`, which rejected the `+24 dB` end of the SPEC's suggested
`-24..+24 dB` range. Now `-60..+24 dB`.

## Defaults we lay down now

All of these live in `config.json` (`src/Config/AppConfig.h`), so changing them
after real hardware becomes available is a settings change, not a code change.

| Setting | Default | Basis |
|---|---|---|
| `audio.inputDeviceId`, `audio.outputDeviceId` | empty | empty means "not selected yet": task 004 supplies enumeration, task 015 lets the operator pick and persist it. We never hardcode a device string the operator has not chosen |
| `audio.sampleRate` | 48000 | SPEC "Audio": preferred 48 kHz |
| `audio.bufferFrames` | 480 | 10 ms at 48 kHz, our choice inside the validated 64..2048 range; the SPEC leaves the block size to the implementation, and the installed driver offers 256 (measured), so a fallback is expected and logged |
| `audio.inputChannel`, `audio.outputChannel` | 1, 1 | SPEC wants mono for translation; SPEC "Input Channel Selection" makes the number an operator choice (its example is 17), 1 is only the neutral first channel. One-based indexing is our convention, documented in `audio::DeviceRequest` |
| `audio.inputGainDb`, `audio.outputGainDb` | 0.0 | neutral; task 006 adds gain DSP |
| `translation.jitterBufferMs` | 250 | provisional engineering value derived from the live wire facts (protocol doc section 15: bursts up to 2x realtime, post-close drain ~4.7x; jitter headroom above pre-roll ≈ one pre-roll), replacing the 120 ms guess whose bursts overflowed. The rig sweep (`docs/rig-checklist.md` section 7) confirms it on a real device; still **not an end-to-end measurement** |
| `ndi.enabled` | false | subtitles are opt-in (SPEC) |
| `diagnostics.logLevel` | "info" | operator-safe default |

Two of these defaults are now known to be **not offered by this driver** and must be
read from the device rather than trusted: the requested 480 frames come back as 256
(only one buffer size is offered), and channel indices 1/1 are inside a 32x32 device
but carry no audio without a server and rack routing. `audio.inputChannel` /
`audio.outputChannel` stay 1 until a human picks real SoundGrid channels - the driver
accepts any index from 1 to 32, so a wrong value fails silently rather than loudly.

Still **not** defaulted, because there is still no evidence for them:

* which physical console port carries the FOH feed. The driver does name its channels,
  but only as `SoundGrid 1` to `SoundGrid 32` - that identifies a driver stream, not the
  console patch behind it, so choosing the channel stays an operator decision and the
  task 015 picker must present it that way;
* end-to-end latency to the audience (needs the whole chain, see task 018);
* the NDI receiver behaviour (task 016).

Anything the UI shows as a number must come from a live driver query at run time, not
from this document (AGENTS.md 19). The 32x32/256/384/256 figures above are a record of
one measurement of this installation, not a compile-time constant, and no source file
contains them.

## Required behaviour, and what task 004 proved

Verified on this machine (10 cycles in Debug, 5 and 3 in Release):

1. enumeration lists `Waves SoundGrid ASIO` and `--verify` matches it one-to-one with
   the `HKLM\SOFTWARE\ASIO` registration;
2. `open()` succeeds with no server, `start()` delivers callbacks, `stop()` and
   `close()` return the backend to `closed`, repeatedly, without crash, hang or leak;
3. a request for 480 frames falls back to 256 and the fallback is logged;
4. an unregistered or missing device is refused by name ("is not a registered ASIO
   device") - the code never picks another device instead (SPEC "ASIO Device
   Selection");
5. settings naming two different ASIO devices for input and output are refused at
   start-up rather than silently using one of them.

What the measurement does **not** prove: that real console audio reaches channel 1, and
that the translated signal leaves on the chosen output. Without a server the driver
runs on silence, so all of it stays a human check.

## The device is exclusive: capability answers depend on who else is running

Measured twice, with different answers, and the difference was not in our code:

| When | Other ASIO hosts running | What the driver reported to the same code |
|---|---|---|
| 2026-10-01, 18:06-20:34 | none | 32 in / 32 out channels, rates `[44100, 48000, 88200, 96000]`, buffer sizes `[256]`, latency 384/256 samples, `open()` and 10+4 lifecycle cycles OK |
| 2026-10-02, 09:46-09:50 | `SoundGrid QRec`, `SoundGrid Driver Control Panel`, `WavesLocalServer`, `WavesPluginServer` (started 21:41 the previous evening) | **no** sample rates, **no** buffer sizes, **0 channels**; `open()` refused |

So the numbers in the table above are only reproducible while nothing else holds the
device. That is a property of ASIO, not of this application, and it has two consequences
that are now in the code:

* nothing in `src/` uses 32, 256 or 384 as a constant. Every capability comes from a live
  driver query at start-up, so when the answer changes the application's answer changes
  with it;
* the refusal says what the operator can act on. `asio::validateChannelSelection`
  distinguishes "this index is out of range" from "the driver reported no channels at
  all", and the latter names the actual cause: an ASIO device is exclusive - close the
  Control Panel / DAW, or check that a server is configured. Pinned by the test
  "AsioDeviceInfo: a driver with no channels at all says so differently".

## What task 005 (realtime pipeline) proved, and what it still needs

Proven on this machine, without a device:

* the pipeline is real, not a placeholder: input -> ring -> loopback worker -> jitter ->
  output, with a strictly increasing counter ramp verified to arrive at the output in
  order (`AudioLoopback: a ramp written at the input comes back at the output`);
* the realtime rule is enforced twice over: a lexical gate over the bodies of all ten
  callback-reachable functions (`realtime_safety_audit`, with its own self-test that
  injects one violation of each forbidden class), and a measured allocation count of 0
  over 5000 callbacks with the worker thread running (`lingoflow_realtime_tests`);
* overflow and underflow are accounted, not hidden: `captured == forwarded + dropped`
  is asserted after 5000 blocks, and the counters survive `deactivate()` because they
  belong to the engine, not to the buffers;
* silence is the default output: nothing can reach the ASIO output except what was
  written into the jitter buffer, so an unprimed or drained buffer plays silence and
  counts it - never the microphone;
* selecting a device in settings really opens that device in the application: with
  `audio.inputDeviceId = "Waves SoundGrid ASIO"` in the live `config.json`, the app
  logged `opening the audio device selected in settings` and then the driver's own
  refusal, and exited with code 2 (`--smoke` returns 0 only when the start succeeded).

Still a human check, and this is the part no bench here can do: `--loopback` on a machine
where a real console feed reaches the selected input channel. Expected observation there:
`in=` and `out=` levels move together, `out=` lagging by roughly `--jitter` ms,
`underruns` staying at 0 while the source is continuous, and the patched output audible in
the monitor path.

## What task 006 (gain, mute, clipping) proved, and what it adds to the rig check

Proven on this machine, without a device:

* both trims sit where SPEC "Audio Pipeline" puts them - input gain before the ring (the
  translator receives the operator's level, verified by reading the ring back), output gain
  after the jitter buffer (verified on the frames that go to the wire), and the two compose
  so that -6 dB in and +6 dB out returns the original number;
* a gain or mute change glides instead of jumping, measured rather than asserted: cutting a
  500 Hz sine at its peak by 12 dB produces a 0.37 sample step with an instant change and
  nothing above the wave's own slew (~0.033) with the 20 ms glide; a 20 000-block sweep run
  while another thread moves the level stays inside the same bound;
* the callback still allocates nothing while the level changes every block
  (`Realtime path: a gain change costs no allocations and no locks`), and the lexical gate
  now reads the bodies of `GainStage::process` and `GainStage::dbToLinear`, with its
  self-test injecting violations into both so the coverage is proven, not assumed;
* clipping is counted as three different facts (from the device, created by the input trim,
  sent to the audience), so turning the gain down cannot hide a console that already
  arrived at full scale - `attenuation cannot hide clipping that came from the device`;
* refusing to limit is a tested property, not a comment: a sample above full scale passes
  through untouched, and only a value that is not a number at all (an overflowed product)
  is turned into full scale and counted;
* levels, mute state and clipping history survive `deactivate()` and are re-applied by the
  next `activate()`, so a device restart does not reset the room;
* settings really reach the DSP: with `audio.inputGainDb = -6.5, audio.outputGainDb = 3.0`
  in the live `%APPDATA%\LingoFlow\config.json`, `--smoke` logged
  `audio gains in effect: input -6.5 dB, output +3.0 dB, glide 20 ms` and exited 0.

What the rig check (still open from task 005) now has to confirm with ears as well as
numbers:

```powershell
.\lingoflow_asio_probe.exe --loopback "Waves SoundGrid ASIO" --seconds 20 --input 17 --output 3 --jitter 120 --gain-out -6
```

Expected: `in=` and `out=` move together about 120 ms apart, `out=` sits roughly 6 dB below
`in=` with `--gain-out -6`, `appliedGain=` shows 0.0/-6.0 once the glide has landed, and
while dragging nothing is heard but smooth level change - no click, no tick, no step at the
moment the number changes. Pushing `--gain-in` until `clip=out` appears and
`clipping after close ... toAudience>0` is the positive test that the indication works.

## Remaining human checkpoints (hardware)

| Task | What a human must confirm, on a machine with a SoundGrid server |
|---|---|
| 005 | `lingoflow_asio_probe --loopback "<device>"`: real console audio in the selected input channel arrives at the selected output, `in=`/`out=` levels move together with roughly `--jitter` ms of delay, and the engine's underrun counters stay at 0 under load. The driver reports no xrun counter, so ours is the only source of truth |
| 006 | input gain, meters and clipping react to a real signal |
| 012/018 | translated audio leaves on the selected output; end-to-end latency judged by ear against the 0.7-1.5 s target |
| 016 | subtitles visible on a real NDI receiver (Studio Monitor is installed here, so this one is closable locally) |
| 023 | recovery when the server drops mid-show |
| 024 | long-run stability 1h/4h/8h against the real device |
| 027 | final production audit with real routing |

Honest status: **the ASIO device path is verified against the real driver, but audio
actually passing through a SoundGrid system is not**, and cannot be on this PC.
