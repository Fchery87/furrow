cmake_minimum_required(VERSION 3.28)

project(Furrow VERSION 0.0.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(FURROW_BUILD_PLUGIN "Build the plugin. Needs JUCE, which is downloaded at configure time." ON)
option(FURROW_BUILD_TESTS "Build the engine tests." ON)
option(FURROW_LTO "Use link-time optimisation. Slow to link; meant for release builds." OFF)
option(FURROW_SANITIZE "Build the engine tests with address and undefined-behaviour sanitizers." OFF)

include(FetchContent)

# Dependencies are pinned. Archives are checked against their SHA-256; the git dependency is
# pinned to a commit. See plugin/docs/plugin-spec.md for why each one is used.

# The engine: pure C++, no JUCE, so it builds and tests on its own.
add_library(furrow_engine INTERFACE)
target_include_directories(furrow_engine INTERFACE engine/include)
target_compile_features(furrow_engine INTERFACE cxx_std_20)
# The engine must round like SeqCore to the last bit. Fusing a multiply and an add into one
# instruction skips a rounding step, so it is turned off. MSVC does not fuse by default.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(furrow_engine INTERFACE -ffp-contract=off)
endif()

if(FURROW_BUILD_PLUGIN)
    # JUCE 9.0.4 (tag commit 3fb5cbc0340758596dac39f7c8de47d562b4896e), used under AGPLv3.
    FetchContent_Declare(juce
        URL https://github.com/juce-framework/JUCE/archive/refs/tags/9.0.4.tar.gz
        URL_HASH SHA256=e2bee25d5c776118aea550ad168a3dc9520c0669a79bd9f5a72b9f404ee1c40a)
    # clap-juce-extensions publishes from main rather than tagged releases. This commit has JUCE 9
    # support, play-head sample rounding (#187) and split-block transport reporting (#192).
    FetchContent_Declare(clap_juce_extensions
        GIT_REPOSITORY https://github.com/free-audio/clap-juce-extensions.git
        GIT_TAG 7adee3a1bd4684d4caa5601100e364abccff4b4d
        GIT_SUBMODULES_RECURSE TRUE
        EXCLUDE_FROM_ALL)
    FetchContent_MakeAvailable(juce)
    FetchContent_MakeAvailable(clap_juce_extensions)

    juce_add_plugin(FurrowSpike
        COMPANY_NAME "No Chaserz"
        BUNDLE_ID com.nochaserz.furrow-spike
        PLUGIN_MANUFACTURER_CODE Nchz
        PLUGIN_CODE Fspk
        FORMATS VST3
        PRODUCT_NAME "Furrow Spike"
        IS_SYNTH TRUE
        NEEDS_MIDI_INPUT FALSE
        NEEDS_MIDI_OUTPUT TRUE
        IS_MIDI_EFFECT FALSE
        VST3_CATEGORIES Instrument Generator
        COPY_PLUGIN_AFTER_BUILD FALSE)

    clap_juce_extensions_plugin(TARGET FurrowSpike
        CLAP_ID "com.nochaserz.furrow-spike"
        CLAP_FEATURES instrument drum-machine)

    target_sources(FurrowSpike PRIVATE src/SpikeProcessor.cpp)

    target_compile_definitions(FurrowSpike
        PUBLIC
            JUCE_WEB_BROWSER=0
            JUCE_USE_CURL=0
            JUCE_VST3_CAN_REPLACE_VST2=0)

    target_link_libraries(FurrowSpike
        PRIVATE
            furrow_engine
            juce::juce_audio_processors
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags)
    if(FURROW_LTO)
        target_link_libraries(FurrowSpike PUBLIC juce::juce_recommended_lto_flags)
    endif()
endif()

if(FURROW_BUILD_TESTS)
    FetchContent_Declare(Catch2
        URL https://github.com/catchorg/Catch2/archive/refs/tags/v3.16.1.tar.gz
        URL_HASH SHA256=d16bd1b8b2364918bd472cb216394acb93bde6784890df0d91bccfb3f28d777d
        SYSTEM)
    FetchContent_MakeAvailable(Catch2)

    add_executable(furrow_engine_tests
        tests/EngineTests.cpp
        tests/GoldenTests.cpp
        tests/ReaperCaptureTests.cpp)
    target_link_libraries(furrow_engine_tests PRIVATE furrow_engine Catch2::Catch2WithMain)
    target_compile_definitions(furrow_engine_tests PRIVATE FURROW_REPO_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/..")
    if(MSVC)
        target_compile_options(furrow_engine_tests PRIVATE /W4 /WX /permissive- /utf-8)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(furrow_engine_tests PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wfloat-equal -Werror)
        if(FURROW_SANITIZE)
            target_compile_options(furrow_engine_tests PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
            target_link_options(furrow_engine_tests PRIVATE -fsanitize=address,undefined)
        endif()
    endif()

    enable_testing()
    add_test(NAME furrow_engine_tests COMMAND furrow_engine_tests)
endif()
