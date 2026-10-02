# ---------------------------------------------------------------------------
# lingoflow_core: portable core (no JUCE, no network protocol, no UI)
#
# Dependency direction enforced here (AGENTS.md 7):
#   Utils, Config, Diagnostics  <- leaves
#   Audio                       <- Audio + Diagnostics + Utils
#   Translation                 <- Translation + Utils
#   NDI                         <- NDI + Utils
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
    Audio/Asio/AsioDeviceInfo.cpp
    Audio/Asio/AsioDeviceInfo.h
    Audio/Null/NullAudioBackend.cpp
    Audio/Null/NullAudioBackend.h

    Translation/ITranslationBackend.cpp
    Translation/ITranslationBackend.h
    Translation/Null/NullTranslationBackend.cpp
    Translation/Null/NullTranslationBackend.h

    NDI/INdiOutput.cpp
    NDI/INdiOutput.h
    NDI/Null/NullNdiOutput.cpp
    NDI/Null/NullNdiOutput.h

    App/ApplicationController.cpp
    App/ApplicationController.h)

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
        App/Main.cpp)

target_compile_definitions(LingoFlow
    PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:LingoFlow,JUCE_PRODUCT_NAME>"
        JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:LingoFlow,JUCE_VERSION>")

target_link_libraries(LingoFlow
    PRIVATE
        lingoflow::core
        juce::juce_gui_basics
        juce::juce_data_structures
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

if(MSVC)
    target_compile_options(LingoFlow PRIVATE /utf-8 /MP8)
endif()

set_target_properties(LingoFlow PROPERTIES FOLDER "App")
set_target_properties(lingoflow_core PROPERTIES FOLDER "Core")

# ---------------------------------------------------------------------------
# lingoflow_asio_probe - ASIO discovery/lifecycle verification tool (task 004).
#
# This is the only JUCE-dependent target besides the operator app, and the only
# place that links juce_audio_devices. JUCE ASIO support is switched on here with
# JUCE_ASIO=1; JUCE 9 carries the ASIO interface headers itself, and
# third_party/asiosdk holds the full SDK for driver-side/reference work.
# ---------------------------------------------------------------------------
add_executable(lingoflow_asio_probe
    Platform/Asio/AsioDiscovery.cpp
    Platform/Asio/JuceAsioBackend.cpp
    Platform/Asio/AsioProbeMain.cpp)

# JUCE_ASIO=1 is what switches the JUCE ASIO backend on. JUCE 9 carries the ASIO
# interface headers itself (modules/juce_audio_devices/native/asio), the vendored
# third_party/asiosdk is for driver-side and reference host work.
target_compile_definitions(lingoflow_asio_probe
    PRIVATE
        JUCE_ASIO=1
        JUCE_STANDALONE_APPLICATION=1)

target_link_libraries(lingoflow_asio_probe
    PRIVATE
        lingoflow::core
        juce::juce_audio_devices
        juce::juce_events
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

if(MSVC)
    target_compile_options(lingoflow_asio_probe PRIVATE /W4 /utf-8 /MP8)
endif()

set_target_properties(lingoflow_asio_probe PROPERTIES FOLDER "Tools")
