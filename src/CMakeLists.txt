# ---------------------------------------------------------------------------
# lingoflow_core: portable core (no JUCE, no network protocol, no UI)
#
# Dependency direction enforced here (AGENTS.md 7):
#   Utils, Config, Diagnostics  <- leaves
#   Audio                       <- Audio + Diagnostics + Utils
#   Translation                 <- Translation + Utils
#   NDI                         <- NDI + Utils
#   Network                     <- Network + Translation + Security + Utils (task 009;
#                                  a separate target: nlohmann/json and winhttp stay
#                                  private to it, and no portable-core module can see
#                                  the OpenAI protocol)
#   App                         <- everything above (composition root)
# tests/ArchitectureBoundaries.cmake fails the test run if an include leaks
# across those boundaries.
# ---------------------------------------------------------------------------
add_library(lingoflow_core STATIC
    Utils/Log.cpp
    Utils/Log.h

    Config/AppConfig.h
    Config/ConfigSchema.cpp
    Config/ConfigSchema.h
    Config/ConfigStore.cpp
    Config/ConfigStore.h
    Config/ConfigManager.cpp
    Config/ConfigManager.h

    Security/ISecretStore.cpp
    Security/ISecretStore.h

    Diagnostics/DiagnosticsManager.cpp
    Diagnostics/DiagnosticsManager.h

    Audio/AudioTypes.cpp
    Audio/AudioTypes.h
    Audio/IAudioBackend.h
    Audio/AudioEngine.cpp
    Audio/AudioEngine.h
    Audio/AudioJitterBuffer.cpp
    Audio/AudioJitterBuffer.h
    Audio/AudioLoopback.cpp
    Audio/AudioLoopback.h
    Audio/AudioRingBuffer.cpp
    Audio/AudioRingBuffer.h
    Audio/GainStage.cpp
    Audio/GainStage.h
    Audio/LevelMeter.cpp
    Audio/LevelMeter.h
    Audio/Asio/AsioDeviceInfo.cpp
    Audio/Asio/AsioDeviceInfo.h
    Audio/Null/NullAudioBackend.cpp
    Audio/Null/NullAudioBackend.h

    Translation/ITranslationBackend.cpp
    Translation/ITranslationBackend.h
    Translation/LanguageRegistry.cpp
    Translation/LanguageRegistry.h
    Translation/ReconnectSupervisor.cpp
    Translation/ReconnectSupervisor.h
    Translation/TextPipeline.cpp
    Translation/TextPipeline.h
    Translation/Null/NullTranslationBackend.cpp
    Translation/Null/NullTranslationBackend.h

    NDI/INdiOutput.cpp
    NDI/INdiOutput.h
    NDI/Null/NullNdiOutput.cpp
    NDI/Null/NullNdiOutput.h

    App/ApplicationController.cpp
    App/ApplicationController.h
    App/TranslationStreamer.cpp
    App/TranslationStreamer.h
    App/UiModel.cpp
    App/UiModel.h)

add_library(lingoflow::core ALIAS lingoflow_core)

target_include_directories(lingoflow_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(lingoflow_core PUBLIC cxx_std_20)

# nlohmann/json is a PRIVATE dependency: it stays inside Config/*.cpp, so no other
# module (and no UI code) can start parsing JSON.
target_link_libraries(lingoflow_core PRIVATE nlohmann_json::nlohmann_json)

if(MSVC)
    target_compile_options(lingoflow_core PRIVATE /W4 /utf-8 /MP8)
else()
    target_compile_options(lingoflow_core PRIVATE -Wall -Wextra)
endif()

# ---------------------------------------------------------------------------
# lingoflow_platform_asio - the JUCE-backed ASIO adapter (task 004/005).
#
# A separate target because it is the only code that links juce_audio_devices:
# the portable core stays JUCE-free, and both consumers (the operator app and the
# probe tool) inherit the same JUCE_ASIO=1 setting. That matters: JUCE compiles the
# module sources per target, so compiling JuceAsioBackend with ASIO on and the app
# with ASIO off would link two different definitions of the same classes.
# ---------------------------------------------------------------------------
add_library(lingoflow_platform_asio STATIC
    Platform/Asio/AsioDiscovery.cpp
    Platform/Asio/AsioDiscovery.h
    Platform/Asio/JuceAsioBackend.cpp
    Platform/Asio/JuceAsioBackend.h
    Platform/Asio/JuceAsioCommon.h)

target_compile_definitions(lingoflow_platform_asio PUBLIC JUCE_ASIO=1)

target_link_libraries(lingoflow_platform_asio
    PUBLIC
        lingoflow::core
        juce::juce_audio_devices
        juce::juce_events)

if(MSVC)
    target_compile_options(lingoflow_platform_asio PRIVATE /W4 /utf-8 /MP8)
endif()

set_target_properties(lingoflow_platform_asio PROPERTIES FOLDER "Core")

# ---------------------------------------------------------------------------
# lingoflow_network - the OpenAI realtime translation backend (task 009).
#
# A separate target because it owns the protocol: nlohmann/json is PRIVATE
# here (only Network/*.cpp may parse JSON - same rule the Config module has),
# and winhttp is PUBLIC because every consumer of the production transport
# links that system library. Nothing below this target may include anything
# from it except the App composition root (tests/ArchitectureBoundaries.cmake).
# Windows-only: the transport is winhttp, the product is a Windows app.
# ---------------------------------------------------------------------------
if(WIN32)
    add_library(lingoflow_network STATIC
        Network/IWebSocketTransport.h
        Network/Base64.cpp
        Network/Base64.h
        Network/PcmResampler.cpp
        Network/PcmResampler.h
        Network/OpenAIRealtimeBackend.cpp
        Network/OpenAIRealtimeBackend.h
        Network/WinHttpTransport.cpp
        Network/WinHttpTransport.h)

    target_link_libraries(lingoflow_network
        PUBLIC
            lingoflow::core
            winhttp
        PRIVATE
            nlohmann_json::nlohmann_json)

    if(MSVC)
        target_compile_options(lingoflow_network PRIVATE /W4 /utf-8 /MP8)
    endif()

    set_target_properties(lingoflow_network PROPERTIES FOLDER "Core")

    # ---------------------------------------------------------------------------
    # lingoflow_openai_probe - live validation tool (task 009 HUMAN CHECKPOINT).
    # Links the exact backend the operator app will use, so what the probe shows
    # on real traffic is what the product does.
    # ---------------------------------------------------------------------------
    add_executable(lingoflow_openai_probe
        Network/OpenAIProbeMain.cpp)

    target_link_libraries(lingoflow_openai_probe
        PRIVATE
            lingoflow_network
            advapi32)

    if(MSVC)
        target_compile_options(lingoflow_openai_probe PRIVATE /W4 /utf-8 /MP8)
    endif()

    set_target_properties(lingoflow_openai_probe PROPERTIES FOLDER "Tools")
endif()

# ---------------------------------------------------------------------------
# Operator application (JUCE GUI shell only at this stage)
# ---------------------------------------------------------------------------
juce_add_gui_app(LingoFlow
    PRODUCT_NAME "LingoFlow"
    BUNDLE_ID "com.lingoflow.app"
    COMPANY_NAME "LingoFlow"
    VERSION "${PROJECT_VERSION}"
    NEEDS_WEB_BROWSER FALSE
    NEEDS_CURL FALSE)

juce_generate_juce_header(LingoFlow)

target_sources(LingoFlow
    PRIVATE
        App/Main.cpp
        App/OperatorWindow.cpp
        App/OperatorWindow.h)

target_compile_definitions(LingoFlow
    PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:LingoFlow,JUCE_PRODUCT_NAME>"
        JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:LingoFlow,JUCE_VERSION>")

target_link_libraries(LingoFlow
    PRIVATE
        lingoflow::core
        lingoflow_platform_asio
        juce::juce_gui_basics
        juce::juce_data_structures
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

if(MSVC)
    target_compile_options(LingoFlow PRIVATE /utf-8 /MP8)
endif()

# Task 012: the operator application mounts the production translation chain -
# OpenAIRealtimeBackend inside the ReconnectSupervisor. The Network target is
# Windows-only (winhttp), and Main.cpp only references it under this define, so
# a build without it keeps running on the Null backend instead of failing.
if(WIN32)
    target_link_libraries(LingoFlow PRIVATE lingoflow_network advapi32)
    target_compile_definitions(LingoFlow PRIVATE LINGOFLOW_WITH_OPENAI_BACKEND=1)
endif()

set_target_properties(LingoFlow PROPERTIES FOLDER "App")
set_target_properties(lingoflow_core PROPERTIES FOLDER "Core")
set_target_properties(lingoflow_platform_asio PROPERTIES FOLDER "Core")

# ---------------------------------------------------------------------------
# lingoflow_asio_probe - ASIO discovery/lifecycle/loopback verification tool.
#
# It links the same adapter the operator app links, so whatever this tool proves
# about a device is what the product actually does. JUCE_ASIO=1 arrives through
# lingoflow_platform_asio.
# ---------------------------------------------------------------------------
add_executable(lingoflow_asio_probe
    Platform/Asio/AsioProbeMain.cpp)

target_compile_definitions(lingoflow_asio_probe
    PRIVATE
        JUCE_STANDALONE_APPLICATION=1)

target_link_libraries(lingoflow_asio_probe
    PRIVATE
        lingoflow_platform_asio
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

if(MSVC)
    target_compile_options(lingoflow_asio_probe PRIVATE /W4 /utf-8 /MP8)
endif()

set_target_properties(lingoflow_asio_probe PROPERTIES FOLDER "Tools")
