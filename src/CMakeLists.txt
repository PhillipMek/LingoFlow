# ---------------------------------------------------------------------------
# liveai_core: portable core (no JUCE, no network protocol, no UI)
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
add_library(liveai_core STATIC
    Utils/Log.cpp
    Utils/Log.h

    Config/ConfigManager.cpp
    Config/ConfigManager.h

    Diagnostics/DiagnosticsManager.cpp
    Diagnostics/DiagnosticsManager.h

    Audio/AudioTypes.cpp
    Audio/AudioTypes.h
    Audio/IAudioBackend.h
    Audio/AudioEngine.cpp
    Audio/AudioEngine.h
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

add_library(liveai::core ALIAS liveai_core)

target_include_directories(liveai_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(liveai_core PUBLIC cxx_std_20)

if(MSVC)
    target_compile_options(liveai_core PRIVATE /W4 /utf-8 /MP8)
else()
    target_compile_options(liveai_core PRIVATE -Wall -Wextra)
endif()

# ---------------------------------------------------------------------------
# Operator application (JUCE GUI shell only at this stage)
# ---------------------------------------------------------------------------
juce_add_gui_app(LiveAIInterpreter
    PRODUCT_NAME "Live AI Interpreter"
    BUNDLE_ID "com.liveai.interpreter"
    COMPANY_NAME "Live AI Interpreter"
    VERSION "${PROJECT_VERSION}"
    NEEDS_WEB_BROWSER FALSE
    NEEDS_CURL FALSE)

juce_generate_juce_header(LiveAIInterpreter)

target_sources(LiveAIInterpreter
    PRIVATE
        App/Main.cpp)

target_compile_definitions(LiveAIInterpreter
    PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:LiveAIInterpreter,JUCE_PRODUCT_NAME>"
        JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:LiveAIInterpreter,JUCE_VERSION>")

target_link_libraries(LiveAIInterpreter
    PRIVATE
        liveai::core
        juce::juce_gui_basics
        juce::juce_data_structures
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

if(MSVC)
    target_compile_options(LiveAIInterpreter PRIVATE /utf-8 /MP8)
endif()

set_target_properties(LiveAIInterpreter PROPERTIES FOLDER "App")
set_target_properties(liveai_core PROPERTIES FOLDER "Core")
