# LingoFlow — Specification v2.1

## Product
Professional Windows application for live-event simultaneous interpretation.

```text
FOH/Console
  -> Waves SoundGrid
  -> Waves SoundGrid Driver / ASIO
  -> LingoFlow
  -> ASIO / SoundGrid
  -> FOH/Console
  -> Audience headphone system
```

## Platform
MVP:
- Windows 10/11 x64
- C++20
- JUCE
- CMake
- ASIO
- Waves SoundGrid Driver
- OpenAI Realtime API
- NDI subtitle output

Future macOS/Linux must not require rewriting translation core.

## Audio
Preferred:
- 48 kHz
- float32
- mono translation input/output

ASIO must:
- enumerate devices;
- select/open/close/start/stop;
- expose sample rate, buffer size, channel counts;
- select input/output channels.

Do not implement SoundGrid protocol.

## Realtime
Audio callback:
- no allocation;
- no blocking;
- no network;
- no filesystem;
- no JSON;
- no GUI.

Use lock-free/preallocated buffers.

## DSP
MVP:
- input gain;
- output gain;
- meters;
- clipping indication;
- ring buffers;
- jitter buffer;
- format conversion when needed.

Future architecture:
- HPF;
- gate;
- limiter;
- AGC.

## Translation
Persistent streaming session. Never implement sentence-at-a-time request/wait/play.

MVP:
- English -> Russian
- Russian -> English

Architecture must support backend-supported languages.

## Languages
UI has Input Language and Output Language dropdowns.
Populate via capabilities or versioned manifest.
Validate unsupported pairs.

## Text
Expose partial/final text events where supported.
Pipeline:
- typed events;
- history;
- timestamps/sequence where available;
- UI model.

Text processing never blocks audio.

## NDI
Independent NDI subtitle output:
- enable/disable;
- output name;
- translated text;
- partial/final behavior;
- reconnect/errors.

NDI failure must not stop audio.

## UI
Professional dark/neutral operator UI.
Main screen:
- ASIO/OpenAI/network state;
- input/output language;
- device/input/output;
- meters;
- input/output gain;
- buffer;
- estimated latency;
- underruns;
- Start/Stop.

Single Settings area.
Separate Diagnostics.

## API key
API key can be entered directly in UI.
Production: Windows secure storage, masked UI, never config/log.
Development: environment variable allowed.

## Configuration
Central Settings:
- audio;
- channels;
- gain;
- languages;
- translation instructions;
- model/config identifiers;
- jitter buffer;
- NDI;
- diagnostics.

Secrets separate.

## Translation instructions
Professional simultaneous interpreter instructions:
- preserve meaning, names, numbers, terminology, proper nouns and intent;
- do not summarize/add explanations/comment;
- prioritize low latency without sacrificing quality.

Instructions configurable.

## Glossary
MVP may omit editor, but architecture must support terms and preferred translations.

## Latency
Goal: approximately 0.7–1.5 s where realistically achievable.
Never fabricate measurements.
Show measurable components and clearly label estimates.

## Reliability
Network failure must not close ASIO.
Reconnect and recreate session.
NDI failure must not stop audio.
Recoverable runtime errors must not crash the application.

## Diagnostics
Track:
- ASIO;
- levels;
- clipping;
- ring/jitter fill;
- underruns/overruns;
- OpenAI state;
- reconnect count;
- session duration;
- latency;
- NDI.

Diagnostics export contains no secrets.

## Developer mode
Support:
- WAV input;
- test tone;
- WAV output;
- mock translation;
- timing/buffer diagnostics;
- reconnect tests.

## Tests
Unit:
ring buffer, jitter buffer, gain, format conversion, config, language registry, translation config, text pipeline, latency.

Integration:
WAV -> engine -> mock backend -> output.

Real integration:
ASIO/SoundGrid, OpenAI, NDI.

Long run:
1h, 4h, 8h+.

## Security
Never commit keys, credentials, real .env, private audio.

## Suggested structure
```text
src/
  App/
  Audio/
  Audio/ASIO/
  Translation/
  Network/
  Text/
  NDI/
  UI/
  Config/
  Diagnostics/
  Security/
  Utils/
tests/
docs/
resources/
third_party/
```

## Production MVP
Must:
1. start;
2. discover SoundGrid ASIO;
3. select input/output;
4. start audio;
5. connect to current OpenAI realtime translation service;
6. select supported languages;
7. stream input;
8. receive translated audio;
9. output translated audio;
10. show meters/status/latency;
11. optionally output translated text to NDI;
12. stop safely.
