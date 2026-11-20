# LingoFlow
## Technical Specification v2.1

**Status:** Development Specification  
**Target Platform:** Windows 10/11 x64  
**Primary Audio Driver:** Waves SoundGrid ASIO Driver V15.x  
**Translation Backend:** OpenAI GPT-Realtime-Translate  
**Primary Use Case:** Real-time simultaneous speech translation for live events  
**Initial Production Scenario:** English ↔ Russian  
**Architecture:** Modular, real-time safe, extensible to additional languages and translation providers

---

# 1. Purpose

The application provides real-time simultaneous speech translation for live events.

The application receives speech from a professional digital audio console through the Waves SoundGrid ASIO Driver, sends the audio stream to OpenAI GPT-Realtime-Translate, receives translated speech, and returns the translated audio to a separate SoundGrid output channel.

In parallel, the translated text is processed independently and can be:

- displayed in the application UI;
- stored in the translation history;
- sent to an NDI subtitle output;
- used by additional text-output backends in the future.

The system must prioritize:

1. Minimum practical end-to-end latency;
2. Translation quality;
3. Audio reliability;
4. Continuous operation;
5. Simple operator workflow;
6. Clear diagnostics;
7. Extensibility.

---

# 2. Core Concept

The application is not a SoundGrid network implementation.

SoundGrid communication is handled exclusively through the Waves ASIO driver.

The application sees the audio interface as a standard ASIO device.

```text
Digital Console
      │
      ▼
Waves SoundGrid Network
      │
      ▼
Waves SoundGrid ASIO Driver
      │
      ▼
LingoFlow
      │
      ├──────────────► OpenAI GPT-Realtime-Translate
      │                       │
      │                       ├── translated audio
      │                       └── translated text
      │
      ▼
Waves SoundGrid ASIO Output
      │
      ▼
Digital Console
      │
      ▼
Attendee Headphones
```

---

# 3. Supported Operating System

Production target:

- Windows 10 64-bit;
- Windows 11 64-bit.

Development can be performed on macOS for most non-hardware components.

Actual SoundGrid/ASIO integration testing must be performed on Windows with the target Waves driver.

---

# 4. Hardware Requirements

Recommended production computer:

- modern Intel/AMD CPU;
- minimum 4 physical CPU cores;
- 8 GB RAM minimum;
- 16 GB recommended;
- SSD;
- wired Ethernet;
- separate network interfaces recommended.

Recommended network architecture:

```text
NIC 1
  │
  └── SoundGrid network
          │
          ├── Waves hardware
          └── SoundGrid ASIO

NIC 2
  │
  ├── Internet
  │
  └── OpenAI API / NDI production network
```

SoundGrid traffic should not share the same network interface with unrelated network traffic when this can be avoided.

---

# 5. Translation Languages

The application must NOT be architecturally limited to English ↔ Russian.

The UI must provide two independent language selectors:

```text
Input Language
[ English ▼ ]

Output Language
[ Russian ▼ ]
```

The selected language pair is provided to the translation backend when starting a translation session.

The application must support:

- configurable input language;
- configurable output language;
- validation of supported language pairs;
- future expansion without changing the core audio architecture.

English ↔ Russian remains the initial production scenario, not a hardcoded architectural limitation.

---

# 6. Language Registry

The application must maintain a dedicated `LanguageRegistry`.

Example:

```cpp
struct LanguageInfo
{
    std::string code;
    std::string displayName;
    bool inputSupported;
    bool outputSupported;
};
```

Example API:

```cpp
class LanguageRegistry
{
public:
    std::vector<LanguageInfo> getInputLanguages() const;
    std::vector<LanguageInfo> getOutputLanguages() const;

    bool isInputSupported(const std::string& language) const;
    bool isOutputSupported(const std::string& language) const;

    bool isPairSupported(
        const std::string& inputLanguage,
        const std::string& outputLanguage) const;
};
```

The UI must obtain language lists from this registry rather than maintaining a hardcoded list.

---

# 7. Model Capabilities

The translation backend must expose capabilities to the application.

```cpp
struct TranslationCapabilities
{
    std::string model;

    std::vector<LanguageInfo> inputLanguages;
    std::vector<LanguageInfo> outputLanguages;

    bool supportsAudioInput;
    bool supportsAudioOutput;
    bool supportsTextOutput;
};
```

The application must not assume that `/v1/models` itself contains the complete language capability list.

The architecture should support:

1. local versioned capability manifest;
2. future provider capability endpoint;
3. provider-specific capability adapter.

Example:

```text
OpenAI Provider
      │
      ▼
Capability Adapter
      │
      ▼
TranslationCapabilities
      │
      ▼
LanguageRegistry
      │
      ▼
UI dropdowns
```

This prevents the UI from being coupled to a specific API mechanism.

---

# 8. Language Capability Manifest

A versioned capability file should be supported.

Example:

```text
resources/
└── languages/
    └── gpt-realtime-translate.json
```

Example structure:

```json
{
    "model": "gpt-realtime-translate",
    "version": 1,
    "inputLanguages": [],
    "outputLanguages": []
}
```

The exact language list must be treated as model capability data rather than permanently embedded in source code.

When OpenAI changes model capabilities, the manifest can be updated independently from the UI logic.

---

# 9. Language Selection Validation

Before a session starts:

1. Input language must be selected;
2. Output language must be selected;
3. Both must be supported by the selected model;
4. The selected pair must be valid;
5. The model must support audio translation.

The UI must display:

```text
Pair status:
● Supported
```

or:

```text
Pair status:
✕ Unsupported language pair
```

`Start` must be disabled for an unsupported pair.

Same-language translation should be disabled unless explicitly implemented as a future passthrough mode.

---

# 10. OpenAI Translation Backend

Initial provider:

**OpenAI GPT-Realtime-Translate**

The implementation must use the current official realtime translation API rather than attempting to emulate or reproduce the service protocol.

The provider layer must isolate OpenAI-specific functionality from the rest of the application.

---

# 11. Translation Provider Interface

The application must expose a provider-independent interface.

Example:

```cpp
class ITranslationBackend
{
public:
    virtual ~ITranslationBackend() = default;

    virtual bool connect(
        const TranslationConfig& config) = 0;

    virtual void disconnect() = 0;

    virtual void sendAudio(
        const AudioChunk& audio) = 0;

    virtual TranslationCapabilities
        getCapabilities() const = 0;

    virtual void setTextCallback(
        TextCallback callback) = 0;

    virtual void setAudioCallback(
        AudioCallback callback) = 0;

    virtual void setErrorCallback(
        ErrorCallback callback) = 0;
};
```

The audio callback and text callback must be independent.

---

# 12. Translation Configuration

```cpp
struct TranslationConfig
{
    std::string model;

    std::string inputLanguage;
    std::string outputLanguage;

    std::string instructions;

    int inputSampleRate;
    int outputSampleRate;

    int jitterBufferMs;
};
```

The configuration must not contain the raw API key.

Credentials are handled separately by the security subsystem.

---

# 13. API Key Management

The API key must be configurable directly through the application UI.

Settings:

```text
OpenAI

API Key
[ ••••••••••••••••••• ]

[Show]
[Save]
[Clear]

Status:
● Configured
```

The full key must never be displayed by default.

The application must never log the API key.

---

# 14. Secure Credential Storage

The API key must not be stored in the ordinary JSON configuration file.

On Windows, the implementation should use a secure OS-backed mechanism such as:

- Windows Credential Manager;
- DPAPI-backed protected storage.

Architecture:

```cpp
class ICredentialStore
{
public:
    virtual bool save(
        const std::string& key,
        const std::string& value) = 0;

    virtual std::optional<std::string> load(
        const std::string& key) = 0;

    virtual bool remove(
        const std::string& key) = 0;
};
```

Production priority:

```text
UI credential store
        ↓
Secure OS storage
```

Developer fallback:

```text
OPENAI_API_KEY environment variable
```

Plain-text API keys in `config.json` are prohibited.

---

# 15. API Connection Test

Settings must provide:

```text
[ Test Connection ]
```

The test must:

- verify the API key;
- verify connectivity;
- verify selected model availability where possible;
- not start the audio translation pipeline;
- not send microphone/console audio.

Result:

```text
● Connection successful
```

or:

```text
✕ Connection failed
Reason: ...
```

---

# 16. Audio Input

Audio input comes from ASIO.

Example:

```text
Waves SoundGrid ASIO Driver
        │
        ▼
Input Channel N
        │
        ▼
Audio Callback
```

The application must not implement the SoundGrid network protocol.

---

# 17. ASIO Device Selection

The application must display available ASIO devices.

Example:

```text
ASIO Device
[ Waves SoundGrid ASIO Driver ▼ ]
```

The selected device must be saved in configuration.

If the configured device is unavailable:

```text
ASIO device unavailable
```

and the application must not silently select another device.

---

# 18. Input Channel Selection

The operator must select the SoundGrid input channel.

```text
Input Channel
[ 17 ▼ ]
```

Channel availability depends on the selected ASIO driver configuration.

The selected channel must be visible on the main screen.

---

# 19. Input Gain

The application must provide a digital input gain control before the audio is sent to the translation backend.

Example:

```text
Input Gain
[-6.0 dB] ─────●────────
```

Controls:

- slider;
- numeric value;
- reset to `0 dB`;
- mute.

Suggested initial range:

```text
-24 dB … +24 dB
```

The range should remain configurable.

Important:

**Input Gain is digital gain inside the application.**

It is not hardware microphone preamp control.

Hardware preamp control remains the responsibility of the digital console.

---

# 20. Input Gain DSP Requirements

Gain processing must be real-time safe.

Example:

```cpp
class InputGainProcessor
{
public:
    void setGainDb(float gainDb);
    void setMuted(bool muted);

    void process(
        float* buffer,
        size_t sampleCount);
};
```

The ASIO callback must not:

- allocate memory;
- acquire blocking mutexes;
- perform network operations;
- perform file I/O;
- perform UI operations.

Gain changes should use atomic or lock-free state where practical.

---

# 21. Audio Preprocessing

Optional preprocessing may include:

- DC offset removal;
- channel conversion;
- sample-rate conversion if required;
- controlled gain;
- format conversion.

Voice enhancement should not be overly aggressive.

The original speech must remain natural and intelligible.

---

# 22. Audio Ring Buffer

The ASIO callback writes audio to a lock-free or appropriately real-time-safe ring buffer.

```text
ASIO Callback
     │
     ▼
Input Gain
     │
     ▼
Ring Buffer
     │
     ▼
Translation Worker
```

The network layer must never execute directly inside the ASIO callback.

---

# 23. OpenAI Audio Streaming

Audio is streamed continuously to the realtime translation backend.

The system must not wait for a complete sentence before sending audio.

Primary goal:

```text
speech starts
     ↓
audio chunks sent immediately
     ↓
translation begins while speaker continues
```

---

# 24. Audio Chunking

Chunk size must be configurable or internally optimized.

The application should avoid:

- excessively small network messages;
- unnecessarily large buffering;
- sentence-level batching.

The implementation should provide low-latency streaming while avoiding excessive CPU/network overhead.

---

# 25. Translation Audio Output

Translated audio returned by OpenAI is placed into an output buffer.

```text
OpenAI
   │
   ▼
Translated Audio
   │
   ▼
Output Jitter Buffer
   │
   ▼
ASIO Output
```

The output pipeline must be independent from the text pipeline.

---

# 26. Output Channel

The operator selects the destination SoundGrid channel.

```text
Output Channel
[ 25 ▼ ]
```

The translated audio must be written only to the configured output channel.

---

# 27. Output Gain

Output gain must be available separately from input gain.

```text
Output Gain
[ 0.0 dB ]
```

This allows the operator to compensate for differences between translated and source signal levels without changing the input level sent to the translator.

---

# 28. Output Jitter Buffer

The output must include a configurable jitter buffer.

Suggested initial range:

```text
20–500 ms
```

The system must balance:

- minimum latency;
- resistance to network jitter;
- avoidance of audio underruns.

The actual default must be determined during testing.

---

# 29. Audio Pipeline

Final audio architecture:

```text
ASIO Input
    │
    ▼
Input Gain
    │
    ▼
Ring Buffer
    │
    ▼
OpenAI Realtime Translation
    │
    ▼
Translated Audio
    │
    ▼
Jitter Buffer
    │
    ▼
Output Gain
    │
    ▼
ASIO Output
```

No text processing may block this pipeline.

---

# 30. Text Translation Pipeline

Translated text must be processed independently from translated audio.

```text
OpenAI
   │
   ├──────────────► Audio
   │
   └──────────────► Text Deltas
                         │
                         ▼
                  Text Aggregator
                         │
              ┌──────────┼──────────┐
              ▼          ▼          ▼
             UI         NDI       History
```

The text path must never block the audio callback or audio output worker.

---

# 31. Text Event

Use a structured text event instead of passing plain strings.

Example:

```cpp
struct TranslationTextEvent
{
    uint64_t sequence;
    uint64_t timestamp;

    std::string text;

    bool isPartial;
    bool isFinal;

    std::string inputLanguage;
    std::string outputLanguage;
};
```

This allows the same translation text to be used by:

- UI;
- NDI;
- logging;
- future subtitle systems.

---

# 32. Text Aggregator

OpenAI text deltas must be aggregated into human-readable segments.

Responsibilities:

- concatenate deltas;
- maintain partial text;
- detect finalized segments;
- assign timestamps;
- maintain sequence numbers;
- prevent duplicate output;
- expose current translation state.

Example:

```cpp
class TextAggregator
{
public:
    void onDelta(const TranslationTextEvent& event);

    std::string getCurrentText() const;

    std::vector<TranslationTextEvent>
        getRecentSegments() const;
};
```

---

# 33. Main UI — Translation Text

The main screen must show the current translated text.

Example:

```text
LIVE TRANSLATION
────────────────────────────────

The translated speech appears here
in real time...

────────────────────────────────
```

The display should distinguish:

- current partial translation;
- finalized text.

The UI must not update on every individual network event if this causes excessive rendering.

UI update frequency should be throttled independently of the audio pipeline.

---

# 34. Translation History

The application may maintain a short in-memory translation history.

Example:

```text
[12:41:03] Good evening everyone.
[12:41:08] Welcome to today's conference.
[12:41:15] We are going to begin shortly.
```

History must be bounded to prevent unbounded memory usage.

Long-term recording/export is a separate feature.

---

# 35. NDI Subtitle Output

The application must support NDI-based subtitle output.

The purpose is to allow translated text to be delivered to production systems such as:

- OBS;
- vMix;
- NDI-compatible graphics systems;
- other NDI production infrastructure.

NDI must be implemented as a separate module.

---

# 36. NDI Architecture

```text
Translation Text
      │
      ▼
Text Aggregator
      │
      ▼
ICaptionOutput
      │
      ▼
NDI Caption Output
      │
      ▼
Production Network
      │
      ├── OBS
      ├── vMix
      └── NDI-compatible receiver
```

The NDI subsystem must never block the audio path.

---

# 37. Caption Output Interface

The core application should not depend directly on NDI.

Example:

```cpp
class ICaptionOutput
{
public:
    virtual ~ICaptionOutput() = default;

    virtual bool start(
        const CaptionConfig& config) = 0;

    virtual void stop() = 0;

    virtual void submit(
        const TranslationTextEvent& event) = 0;

    virtual bool isConnected() const = 0;
};
```

Implementations can include:

```text
NDI
Local UI
Future WebSocket
Future OSC
Future subtitle file output
```

---

# 38. NDI Caption Modes

The NDI subsystem should support two architectural modes.

## Mode A — NDI Metadata / Timed Text

Translated text is transmitted as NDI metadata using a suitable timed-text XML representation.

Possible standards include:

- TTML;
- IMSC;
- SMPTE-TT;
- other supported timed-text formats.

The exact format should be selected according to the target receiver.

Important:

NDI metadata transmission does not guarantee that every NDI receiver automatically renders captions.

---

## Mode B — NDI Subtitle Video

Optional future/extended mode.

The application renders subtitles into an NDI video stream.

```text
Text
 ↓
Subtitle Renderer
 ↓
Video Frame
 ↓
NDI Video Source
```

This provides a more universal production workflow for systems that expect a video source rather than timed-text metadata.

The subtitle renderer should support:

- font;
- size;
- position;
- alignment;
- maximum lines;
- maximum characters;
- background;
- outline/shadow;
- margins;
- update rate.

Mode B can be implemented after the metadata mode.

---

# 39. NDI Configuration

Settings:

```text
NDI Subtitles
────────────────────────

Enable NDI:
[✓]

Mode:
[ Timed Text ▼ ]

Source Name:
[ LingoFlow ]

Network Interface:
[ Auto ▼ ]

Maximum Lines:
[ 2 ]

Maximum Characters:
[ 120 ]
```

For video mode, additional settings become available.

---

# 40. NDI Network Isolation

NDI traffic should preferably use a production/Internet network interface rather than the SoundGrid NIC.

Example:

```text
NIC 1
└── SoundGrid

NIC 2
├── Internet / OpenAI
└── NDI
```

The application should allow selecting or binding the appropriate network interface where supported by the NDI SDK.

---

# 41. NDI Failure Handling

NDI failure must never stop audio translation.

Example:

```text
OpenAI ─────► Audio ─────► ASIO
    │
    └────────► Text ─────► NDI ✕
```

If NDI becomes unavailable:

- audio continues;
- UI continues;
- NDI status changes to disconnected;
- reconnection may happen automatically;
- no application crash.

---

# 42. Central Configuration

All user-configurable settings must be accessible from one Settings area.

Recommended structure:

```text
Settings
│
├── Audio
│
├── Translation
│
├── OpenAI
│
├── Subtitles / NDI
│
├── Diagnostics
│
└── Advanced
```

The main screen should contain only operational controls required during an event.

---

# 43. Application Configuration

Non-secret settings should be stored in a configuration file.

Example Windows path:

```text
%APPDATA%\LingoFlow\config.json
```

Possible configuration:

```json
{
    "audio": {
        "asioDevice": "",
        "inputChannel": 17,
        "outputChannel": 25,
        "inputGainDb": 0.0,
        "outputGainDb": 0.0
    },

    "translation": {
        "model": "gpt-realtime-translate",
        "inputLanguage": "en",
        "outputLanguage": "ru",
        "jitterBufferMs": 100
    },

    "ndi": {
        "enabled": false,
        "mode": "metadata",
        "sourceName": "LingoFlow"
    }
}
```

The API key must not be present in this file.

---

# 44. Configuration Manager

A single configuration manager must be the source of truth.

```cpp
class ConfigManager
{
public:
    AppConfig load();
    bool save(const AppConfig& config);

    const AppConfig& get() const;

    void set(const AppConfig& config);
};
```

UI, controller and subsystems should not independently maintain their own persistent settings.

---

# 45. Main Window

The main window should be designed for live-event operation.

Recommended layout:

```text
┌──────────────────────────────────────────────┐
│ LIVE AI INTERPRETER                         │
│                                              │
│ Input Language    [ English ▼ ]              │
│ Output Language   [ Russian ▼ ]              │
│ Pair Status       ● Supported                │
│                                              │
│ Input             [ 17 ▼ ]                   │
│ Input Gain        [ -6.0 dB ]                │
│                                              │
│ Output            [ 25 ▼ ]                   │
│ Output Gain       [  0.0 dB ]                │
│                                              │
│ ───────── LIVE TRANSLATION ────────────────  │
│                                              │
│ The translated speech appears here...       │
│                                              │
│                                              │
│ Audio:       ● Running                       │
│ OpenAI:      ● Connected                     │
│ NDI:         ● Connected                     │
│                                              │
│             [ START ]   [ STOP ]             │
└──────────────────────────────────────────────┘
```

---

# 46. Settings Window

The Settings window should provide:

## Audio

- ASIO device;
- sample rate;
- buffer size;
- input channel;
- output channel;
- input gain;
- output gain.

## Translation

- model;
- input language;
- output language;
- translation instructions;
- jitter buffer.

## OpenAI

- API key;
- connection test;
- connection status;
- model status.

## Subtitles / NDI

- enable;
- mode;
- source name;
- network interface;
- caption parameters.

## Diagnostics

- log level;
- audio statistics;
- network statistics;
- latency statistics.

## Advanced

- developer mode;
- experimental features;
- debugging options.

---

# 47. Runtime State

The application must expose a clear runtime state.

Example:

```cpp
enum class InterpreterState
{
    Stopped,
    Starting,
    Running,
    Degraded,
    Error
};
```

Individual subsystem states should also be exposed.

---

# 48. Status Indicators

The main screen should display:

```text
ASIO       ● Connected
OpenAI     ● Connected
Translation● Running
Audio      ● Running
NDI        ● Connected
```

Possible states:

- Connected;
- Connecting;
- Disconnected;
- Error;
- Disabled.

---

# 49. Start Sequence

When the user presses `START`:

```text
1. Load configuration
2. Validate ASIO device
3. Validate input channel
4. Validate output channel
5. Validate input gain
6. Load OpenAI credential
7. Validate API connection
8. Load model capabilities
9. Validate language pair
10. Start translation session
11. Start audio buffers
12. Start text pipeline
13. Start NDI if enabled
14. Begin audio streaming
```

The application must not begin live audio transmission until configuration validation succeeds.

---

# 50. Stop Sequence

When the user presses `STOP`:

```text
1. Stop accepting new audio
2. Finish pending audio buffers
3. Stop translation session
4. Stop text pipeline
5. Stop NDI
6. Release ASIO resources
7. Clear runtime state
```

The application must avoid abrupt audio artifacts where practical.

---

# 51. Error Handling

The system must fail gracefully.

Possible errors:

- ASIO device unavailable;
- invalid channel;
- OpenAI authentication failure;
- network failure;
- unsupported language pair;
- model unavailable;
- audio buffer underrun;
- audio buffer overrun;
- NDI failure.

Errors must be reported in the UI without exposing credentials.

---

# 52. Audio Reliability

Audio must have priority over:

- UI rendering;
- text processing;
- NDI;
- logging;
- statistics;
- diagnostics.

No non-real-time subsystem may block the audio callback.

---

# 53. Network Failure

If OpenAI becomes unavailable:

```text
OpenAI: ✕ Disconnected
Translation: ✕
```

The application must:

- stop feeding invalid output;
- avoid crashing;
- report the error;
- optionally attempt reconnect;
- preserve configuration.

Automatic reconnection should be configurable.

---

# 54. Latency Measurement

The application should measure approximate:

```text
ASIO input
     ↓
input processing
     ↓
network upload
     ↓
translation
     ↓
network download
     ↓
output buffering
     ↓
ASIO output
```

Statistics should include:

- input buffer latency;
- network round-trip indicators where measurable;
- translation response timing;
- output jitter;
- underruns;
- total estimated latency.

Latency must be displayed separately from translation quality metrics.

---

# 55. Diagnostics

Diagnostics screen:

```text
Audio
Input level:       -18.2 dBFS
Peak:               -3.4 dBFS
Underruns:          0
Overruns:           0

Translation
Status:             Running
Text events:        184
Audio chunks:       12,421

Network
Connection:         Stable

NDI
Status:             Connected
Caption events:     96
```

---

# 56. Logging

Logging levels:

```text
ERROR
WARN
INFO
DEBUG
TRACE
```

Production default:

```text
INFO
```

The logger must never record:

- API keys;
- authorization headers;
- raw credentials.

Audio recording must be disabled by default.

---

# 57. Real-Time Threading Model

Recommended architecture:

```text
┌──────────────────────────────┐
│ ASIO Audio Thread            │
│                              │
│ Capture                     │
│ Gain                        │
│ Ring Buffer                 │
└──────────────┬───────────────┘
               │
               ▼
┌──────────────────────────────┐
│ Translation Audio Worker     │
│                              │
│ Audio encoding               │
│ Network transmission         │
└──────────────┬───────────────┘
               │
               ▼
       OpenAI Realtime API
               │
       ┌───────┴────────┐
       │                │
       ▼                ▼
 Audio Worker      Text Worker
       │                │
       ▼                ▼
Jitter Buffer     Text Aggregator
       │          ┌─────┼──────┐
       ▼          ▼     ▼      ▼
    ASIO       UI     NDI   History
```

---

# 58. Threading Rules

The ASIO callback must not:

- allocate;
- lock long-running mutexes;
- access the network;
- access disk;
- invoke UI code;
- perform expensive string operations.

Text processing must occur on a worker thread.

NDI processing must occur on a worker thread.

Logging must be asynchronous where practical.

---

# 59. Project Structure

Recommended project structure:

```text
LingoFlow/
│
├── src/
│   │
│   ├── App/
│   │   ├── Application.cpp
│   │   └── ApplicationController.cpp
│   │
│   ├── Audio/
│   │   ├── AsioDevice.cpp
│   │   ├── AudioEngine.cpp
│   │   ├── RingBuffer.cpp
│   │   ├── InputGainProcessor.cpp
│   │   └── OutputGainProcessor.cpp
│   │
│   ├── Translation/
│   │   ├── ITranslationBackend.h
│   │   ├── TranslationConfig.h
│   │   ├── TranslationCapabilities.h
│   │   ├── TranslationTextEvent.h
│   │   └── OpenAI/
│   │       └── OpenAIRealtimeTranslate.cpp
│   │
│   ├── Language/
│   │   ├── LanguageInfo.h
│   │   ├── LanguageRegistry.cpp
│   │   └── CapabilityManifest.cpp
│   │
│   ├── Text/
│   │   ├── TextAggregator.cpp
│   │   └── TranslationHistory.cpp
│   │
│   ├── Caption/
│   │   ├── ICaptionOutput.h
│   │   └── NDI/
│   │       ├── NdiCaptionOutput.cpp
│   │       ├── NdiMetadataOutput.cpp
│   │       └── NdiVideoSubtitleOutput.cpp
│   │
│   ├── Config/
│   │   ├── AppConfig.h
│   │   └── ConfigManager.cpp
│   │
│   ├── Security/
│   │   ├── ICredentialStore.h
│   │   └── WindowsCredentialStore.cpp
│   │
│   ├── Diagnostics/
│   │   ├── Logger.cpp
│   │   ├── Metrics.cpp
│   │   └── LatencyMonitor.cpp
│   │
│   └── UI/
│       ├── MainWindow.cpp
│       ├── SettingsWindow.cpp
│       ├── AudioPanel.cpp
│       ├── TranslationPanel.cpp
│       └── DiagnosticsPanel.cpp
│
├── resources/
│   └── languages/
│       └── gpt-realtime-translate.json
│
├── tests/
│
├── config/
│
└── CMakeLists.txt
```

---

# 60. Dependency Isolation

The application must isolate external dependencies.

```text
Core
│
├── Audio
├── Translation interfaces
├── Text
├── Configuration
└── Security interfaces

External implementations
│
├── ASIO
├── OpenAI
└── NDI
```

NDI SDK must not be required by the core translation/audio classes.

Recommended build flag:

```text
LIVEAI_ENABLE_NDI=ON
```

---

# 61. NDI SDK

NDI integration should use the official NDI SDK.

The SDK should be isolated in:

```text
src/Caption/NDI/
```

The application should not implement the NDI network protocol itself.

The exact NDI SDK version should be pinned for reproducible builds.

SDK licensing requirements must be respected.

---

# 62. ASIO Integration

ASIO integration should use the standard ASIO interface exposed by the Waves SoundGrid ASIO Driver.

The application must not:

- implement SoundGrid networking;
- communicate directly with SoundGrid hardware;
- depend on undocumented Waves network protocols.

Target validation:

```text
Waves SoundGrid ASIO Driver V15.x
```

---

# 63. Configuration Persistence

Settings should be saved automatically or via an explicit `Save` action.

The application should preserve:

- ASIO device;
- input channel;
- output channel;
- input gain;
- output gain;
- input language;
- output language;
- model;
- translation instructions;
- jitter buffer;
- NDI configuration;
- UI preferences.

The API key is stored separately in secure storage.

---

# 64. Configuration Validation

Before saving or starting:

```text
ASIO device             ✓
Input channel           ✓
Output channel          ✓
Input language          ✓
Output language         ✓
Language pair           ✓
API key                 ✓
Model                   ✓
NDI configuration       ✓
```

Invalid settings should be clearly highlighted.

---

# 65. Translation Instructions

The operator may configure translation instructions.

Example:

```text
Translate naturally and accurately.
Preserve names, numbers and technical terminology.
Do not summarize.
Do not add explanations.
```

Instructions must be treated as configuration rather than embedded permanently in source code.

---

# 66. Translation Quality Requirements

The system should preserve:

- proper names;
- numbers;
- dates;
- technical terms;
- speaker intent;
- sentence meaning.

The system should avoid:

- unnecessary paraphrasing;
- summarization;
- added explanations;
- artificial verbosity.

---

# 67. Text/Audio Synchronization

Translated text and translated audio originate from the same translation session.

Text events should contain timestamps.

The application should attempt to maintain approximate correspondence between:

```text
translated audio
       ↕
translated text
```

The synchronization mechanism must not delay audio merely to improve subtitle timing.

If necessary, subtitles may be delayed independently.

---

# 68. Subtitle Formatting

Subtitle formatting must support:

- maximum lines;
- maximum characters;
- configurable update interval;
- sentence/phrase segmentation;
- line wrapping;
- clearing after timeout.

Example:

```text
Welcome to today's conference.

We are happy to have you here.
```

The subtitle renderer must avoid excessive flicker caused by partial text updates.

---

# 69. NDI Caption Timing

NDI caption events should contain timing information where supported by the selected format.

The system must preserve event ordering.

If the NDI receiver does not support timed text, the application should provide an alternative rendering mode rather than assuming automatic subtitle display.

---

# 70. Security

The application must protect:

- API credentials;
- configuration files;
- diagnostic logs.

The application must not send API credentials over NDI.

NDI should carry only subtitle/text data or rendered subtitle media.

---

# 71. Privacy

The operator must be informed that audio is being transmitted to the configured translation provider.

The application should clearly show:

```text
Audio streaming:
● Active
```

when live audio is being sent.

No audio should be transmitted before `START`.

---

# 72. Crash Recovery

The application must attempt to recover from:

- temporary OpenAI disconnect;
- NDI disconnect;
- transient network errors.

A subsystem failure must not automatically crash the entire application.

---

# 73. Watchdog

A watchdog may monitor:

- ASIO callback health;
- translation worker;
- text worker;
- NDI worker.

If a worker becomes unresponsive, the application should report the condition and attempt controlled recovery where safe.

---

# 74. Testing — Audio

Tests:

- ASIO device enumeration;
- channel selection;
- input gain;
- output gain;
- mute;
- ring buffer;
- underrun;
- overrun;
- sample-rate conversion;
- output routing.

---

# 75. Testing — Translation

Tests:

- API authentication;
- connection;
- language capability loading;
- language validation;
- unsupported pair;
- audio streaming;
- translated audio reception;
- translated text reception;
- reconnect;
- timeout;
- provider error.

---

# 76. Testing — Text Pipeline

Tests:

- partial delta aggregation;
- final delta handling;
- sequence ordering;
- duplicate events;
- timestamp handling;
- history limits;
- UI update throttling.

---

# 77. Testing — NDI

Tests:

- NDI initialization;
- source creation;
- metadata transmission;
- subtitle timing;
- disconnect;
- reconnect;
- invalid configuration;
- network interface selection.

NDI failures must be verified to have no impact on audio translation.

---

# 78. Testing — Security

Tests:

- API key saving;
- API key loading;
- API key clearing;
- masked display;
- absence of key in logs;
- absence of key in JSON configuration;
- environment variable fallback.

---

# 79. Testing — Configuration

Tests:

- save;
- load;
- malformed configuration;
- missing configuration;
- default values;
- migration between configuration versions.

Configuration should include a version number.

Example:

```json
{
    "configVersion": 1
}
```

---

# 80. Performance Testing

The application must be tested under continuous operation.

Minimum recommended soak test:

```text
4+ hours continuous translation
```

Recommended production validation:

```text
8–12 hours
```

Monitor:

- memory growth;
- CPU usage;
- buffer errors;
- network stability;
- text queue growth;
- NDI stability;
- translation reconnects.

---

# 81. Latency Target

The project goal is:

> Minimum practical end-to-end latency without unacceptable degradation of translation quality.

Latency must be optimized through:

- small audio chunks;
- streaming input;
- streaming output;
- minimal local buffering;
- low jitter buffer;
- asynchronous text processing.

Latency optimization must not compromise audio stability.

---

# 82. Reliability Priority

Priority order:

```text
1. Audio integrity
2. Translation continuity
3. Translation quality
4. Latency
5. Text/subtitle presentation
6. UI diagnostics
```

The text/NDI subsystem must always be lower priority than the audio pipeline.

---

# 83. Developer Mode

Developer mode may expose:

- raw latency metrics;
- detailed OpenAI events;
- audio buffer statistics;
- text event statistics;
- NDI events;
- network diagnostics.

Developer mode must not expose credentials.

---

# 84. Build Configuration

Recommended CMake options:

```text
LIVEAI_ENABLE_NDI=ON
LIVEAI_ENABLE_DEV_MODE=OFF
LIVEAI_ENABLE_TESTS=ON
```

Production build:

```text
LIVEAI_ENABLE_DEV_MODE=OFF
```

---

# 85. Continuous Integration

CI should validate at least:

- Windows build;
- unit tests;
- configuration tests;
- text pipeline tests;
- language registry tests.

Actual ASIO/SoundGrid testing requires physical Windows test hardware.

---

# 86. macOS Development

macOS may be used for:

- UI development;
- core C++ development;
- configuration;
- language registry;
- text pipeline;
- provider abstractions;
- unit tests;
- documentation.

Windows is required for:

- Waves SoundGrid ASIO;
- production audio testing;
- final hardware validation.

Possible workflow:

```text
macOS
  ↓
Git repository
  ↓
Windows CI
  ↓
Production Windows machine
  ↓
SoundGrid hardware
```

---

# 87. Deployment

Production package should contain:

```text
LingoFlow.exe
Required runtime libraries
NDI dependencies if enabled
Configuration defaults
Language capability manifest
Documentation
```

The installer must not contain a user's API key.

---

# 88. First Launch

On first launch:

```text
Welcome

1. Select ASIO device
2. Select Input Channel
3. Select Output Channel
4. Enter OpenAI API Key
5. Test connection
6. Select Input Language
7. Select Output Language
8. Configure NDI if required
9. Save configuration
```

The application should not require editing JSON files for normal operation.

---

# 89. Operator Workflow

Normal event workflow:

```text
1. Open application
2. Verify ASIO
3. Verify language pair
4. Verify API connection
5. Verify NDI if enabled
6. Check input level
7. Adjust Input Gain if necessary
8. Press START
9. Monitor status
10. Press STOP after event
```

The operator should not need to interact with configuration files or command-line tools.

---

# 90. Final Architecture

The complete v2.1 architecture:

```text
                       ┌──────────────────────┐
                       │      Main UI         │
                       │                      │
                       │ Input Language  ▼    │
                       │ Output Language ▼    │
                       │ Input Gain           │
                       │ Output Gain          │
                       │ Translation Text     │
                       │ Status               │
                       └──────────┬───────────┘
                                  │
                                  ▼
                       ┌──────────────────────┐
                       │ ApplicationController│
                       └──────────┬───────────┘
                                  │
             ┌────────────────────┼─────────────────────┐
             │                    │                     │
             ▼                    ▼                     ▼
      ┌─────────────┐      ┌──────────────┐     ┌─────────────┐
      │ AudioEngine │      │ Language     │     │ Config      │
      │             │      │ Registry     │     │ Manager     │
      └──────┬──────┘      └──────────────┘     └─────────────┘
             │
             ▼
      ┌─────────────┐
      │ Input Gain  │
      └──────┬──────┘
             │
             ▼
      ┌─────────────┐
      │ Ring Buffer │
      └──────┬──────┘
             │
             ▼
   ┌──────────────────────┐
   │ OpenAI Realtime      │
   │ Translation Backend  │
   └──────────┬───────────┘
              │
       ┌──────┴─────────┐
       │                │
       ▼                ▼
┌──────────────┐  ┌──────────────┐
│ Audio Output │  │ Text Deltas  │
└──────┬───────┘  └──────┬───────┘
       │                 │
       ▼                 ▼
┌──────────────┐  ┌──────────────┐
│ Jitter Buffer│  │ Text         │
│ Output Gain  │  │ Aggregator   │
└──────┬───────┘  └──────┬───────┘
       │                 │
       ▼          ┌──────┼────────────┐
┌──────────────┐  │      │            │
│ ASIO Output  │  ▼      ▼            ▼
└──────┬───────┘ UI     NDI        History
       │
       ▼
SoundGrid Console
       │
       ▼
Attendee Headphones
```

---

# 91. Core Design Principles

The implementation must follow these principles:

### 1. Audio first

Nothing outside the audio pipeline may block real-time audio.

### 2. Provider abstraction

OpenAI-specific code must remain inside the provider implementation.

### 3. No SoundGrid protocol implementation

SoundGrid is accessed through the Waves ASIO driver.

### 4. Languages are capabilities

The application must not be architecturally limited to EN/RU.

### 5. Text and audio are separate

Translated text must never become a source of audio latency.

### 6. NDI is optional

The application must function normally without NDI.

### 7. Secrets are isolated

API keys never belong in ordinary configuration or logs.

### 8. One configuration center

The operator configures the system through one Settings interface.

### 9. Production failures must be isolated

Failure of NDI, UI, logging or text processing must not crash audio.

### 10. Production simplicity

The main screen must remain understandable during a live event.

---

# 92. MVP Scope

The first production-capable MVP should include:

- Windows 10/11;
- Waves SoundGrid ASIO Driver V15.x;
- ASIO input;
- ASIO output;
- input channel selection;
- output channel selection;
- input gain;
- output gain;
- OpenAI API key entered through UI;
- secure credential storage;
- GPT-Realtime-Translate;
- Input Language selector;
- Output Language selector;
- capability-based language validation;
- streaming translated audio;
- translated text stream;
- live text display;
- basic translation history;
- basic NDI timed-text output;
- connection status;
- audio meters;
- latency diagnostics;
- configuration persistence;
- automatic reconnect where safe.

---

# 93. Post-MVP

Potential subsequent features:

- NDI subtitle video renderer;
- advanced subtitle styling;
- multiple simultaneous translation sessions;
- multiple output languages;
- speaker identification;
- translation presets;
- event profiles;
- WebSocket text output;
- OSC integration;
- subtitle recording;
- translation transcript export;
- remote monitoring;
- redundant network connection;
- redundant translation provider.

---

# 94. Acceptance Criteria

The application is considered production-ready when:

### Audio

- ASIO input works reliably;
- ASIO output works reliably;
- input gain works without clicks/artifacts;
- output gain works;
- no continuous underruns occur under normal conditions.

### Translation

- OpenAI connection is stable;
- selected language pair is validated;
- translated audio is returned continuously;
- translation quality is acceptable for live speech.

### Text

- translated text appears in real time;
- partial/final text is handled correctly;
- text does not affect audio latency.

### NDI

- NDI output can be enabled/disabled;
- subtitle text reaches the configured NDI workflow;
- NDI failure does not interrupt audio.

### Security

- API key can be entered through UI;
- API key is stored securely;
- API key is absent from normal configuration;
- API key never appears in logs.

### Configuration

- all normal settings are available through Settings;
- configuration survives application restart;
- invalid configuration is clearly reported.

### Reliability

- application survives long continuous sessions;
- subsystem failures do not cause uncontrolled crashes;
- reconnect behavior is predictable.

---

# 95. Final Product Definition

**LingoFlow v2.1** is a Windows application for professional live-event simultaneous interpretation.

The application receives professional audio through the Waves SoundGrid ASIO Driver, streams speech to OpenAI GPT-Realtime-Translate, returns translated speech to a configurable SoundGrid output, and simultaneously exposes translated text through the application UI and NDI subtitle infrastructure.

The architecture is intentionally separated into:

```text
Audio
Translation
Language Capabilities
Text
Subtitles / NDI
Configuration
Security
Diagnostics
UI
```

This separation is mandatory for maintaining low latency, reliability and future extensibility.

The system must not be architecturally tied to English/Russian, a single output method, or a single translation provider.

The primary production path remains:

```text
SoundGrid
   ↓
ASIO
   ↓
Input Gain
   ↓
OpenAI GPT-Realtime-Translate
   ├──► Translated Audio ──► ASIO ──► SoundGrid ──► Headphones
   │
   └──► Translated Text ──► UI
                         └─► NDI Subtitles
```

**This architecture is the baseline for implementation.**