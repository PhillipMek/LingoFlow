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

## Defaults we lay down now

All of these live in `config.json` (`src/Config/AppConfig.h`), so changing them
after real hardware becomes available is a settings change, not a code change.

| Setting | Default | Basis |
|---|---|---|
| `audio.inputDeviceId`, `audio.outputDeviceId` | empty | empty means "not selected yet"; task 004 fills them from enumeration, task 015 lets the operator pick. We never hardcode a device string we have not seen |
| `audio.sampleRate` | 48000 | SPEC "Audio": preferred 48 kHz |
| `audio.bufferFrames` | 480 | 10 ms at 48 kHz; inside the validated 64..2048 range, SPEC MVP buffer |
| `audio.inputChannel`, `audio.outputChannel` | 1, 1 | SPEC: mono translation input/output, one-based indices |
| `audio.inputGainDb`, `audio.outputGainDb` | 0.0 | neutral; task 006 adds gain DSP |
| `translation.jitterBufferMs` | 120 | engineering starting point for the latency target 0.7-1.5 s, **not a measurement** |
| `ndi.enabled` | false | subtitles are opt-in (SPEC) |
| `diagnostics.logLevel` | "info" | operator-safe default |

Explicitly **not** defaulted, because we have no evidence for them:

* the number of SoundGrid input/output channels;
* the names of those channels;
* driver-reported or measured round-trip latency;
* which physical port carries the FOH feed.

Any of these appearing in the UI as a number would have to come from a real
`ASIOGetChannels`/`ASIOGetBufferSizes`/`ASIOGetLatencies` call, i.e. from task 004
on a machine with a server. Until then the UI shows "no device selected" and
"latency: not measured", never a guess (AGENTS.md 19 forbids hardcoded measured
latency).

## Required behaviour on a server-less machine

Task 004/005 must handle exactly this situation, and it is testable here:

1. enumeration lists "Waves SoundGrid" (registration is present);
2. `open()`/`start()` on it fails or reports zero usable channels - this is the
   expected outcome with no server;
3. the failure is reported to the operator and to diagnostics (`lastError` with
   subsystem "audio"), the application stays alive, other backends (WASAPI in
   developer mode, task 019) keep working;
4. no crash, no retry storm, no blocking wait: the audio thread is never started
   against a device that failed to open.

## Deferred human checkpoints (hardware)

These cannot be closed on this PC. They move to a SoundGrid-equipped machine or to
the venue:

| Task | What a human must confirm |
|---|---|
| 004 | "Waves SoundGrid" opens; reported channel count and names match the rack |
| 005 | ASIO callback runs at the chosen buffer size without xruns; driver-reported latencies are read, not guessed |
| 006 | input gain / meters / clipping behave on a real signal |
| 012/018 | end-to-end latency measured with a real source; the 0.7-1.5 s target is judged by ear |
| 016 | subtitles visible on a real NDI receiver (Studio Monitor is installed here, so this one is closable locally) |
| 023 | recovery with the server unplugged mid-show |
| 024 | long-run stability 1h/4h/8h against the real device |
| 027 | final production audit with real routing |

Until those are done, the project status is honest: **real SoundGrid operation is
unverified**, everything else is testable on this machine.
