cmake_minimum_required(VERSION 3.15)

# P0-1: Default to Release build for performance (Debug builds of JUCE/Tracktion are very slow)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(TARGET_NAME ptLive)

#== Local dependency paths ===================================================
# Default: the libraries sit NEXT TO the project folder (../JUCE-master, ../tracktion_engine-develop,
# ../ASIOSDK). If the project lives somewhere else (e.g. a backup copy), point to them with
#   cmake -DPTLIVE_JUCE_DIR=... -DPTLIVE_TRACKTION_DIR=... -DPTLIVE_ASIO_DIR=...
# (or set the environment variables of the same names). Must come BEFORE project().
foreach(dep JUCE TRACKTION ASIO)
    if(DEFINED ENV{PTLIVE_${dep}_DIR} AND NOT DEFINED PTLIVE_${dep}_DIR)
        set(PTLIVE_${dep}_DIR "$ENV{PTLIVE_${dep}_DIR}")
    endif()
endforeach()

if(NOT PTLIVE_JUCE_DIR)
    set(PTLIVE_JUCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../JUCE-master/JUCE-master)
endif()
if(NOT PTLIVE_TRACKTION_DIR)
    set(PTLIVE_TRACKTION_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../tracktion_engine-develop)
endif()
if(NOT PTLIVE_ASIO_DIR)
    set(PTLIVE_ASIO_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../ASIOSDK)
endif()

# Fail early with a readable message instead of a cascade of CMake errors.
foreach(dep JUCE TRACKTION)
    if(NOT EXISTS "${PTLIVE_${dep}_DIR}")
        message(FATAL_ERROR "PTLIVE_${dep}_DIR not found: ${PTLIVE_${dep}_DIR}\n"
                            "Put the library next to the project folder or pass -DPTLIVE_${dep}_DIR=<path>.")
    endif()
endforeach()
if(NOT EXISTS "${PTLIVE_ASIO_DIR}")
    message(WARNING "PTLIVE_ASIO_DIR not found: ${PTLIVE_ASIO_DIR} (ASIO build may fail)")
endif()

file(READ ${PTLIVE_TRACKTION_DIR}/VERSION.md TE_VERSION)

project(${TARGET_NAME} VERSION ${TE_VERSION})

#== Essential JUCE/plugin flags ==============================================
add_compile_definitions(
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_VST=1
    JUCE_MODAL_LOOPS_PERMITTED=1
    JUCE_ASIO=1
    JUCE_ASIO_USE_EXTERNAL_SDK=1
)

#== Register JUCE modules (from Tracktion's bundled copy or local JUCE) =======
if(EXISTS ${PTLIVE_JUCE_DIR}/../modules/juce)
    set(JUCE_DIR ${PTLIVE_JUCE_DIR}/../modules/juce)
else()
    # Fall back to bundled JUCE in tracktion
    set(JUCE_DIR ${PTLIVE_TRACKTION_DIR}/modules/juce)
endif()

# Enable source groups for IDE
set(JUCE_ENABLE_MODULE_SOURCE_GROUPS ON)

# == ASIO external SDK configuration ========================================
# Define JUCE_ASIO_USE_EXTERNAL_SDK=1 BEFORE adding_subdirectory so JUCE uses our SDK
add_compile_definitions(JUCE_ASIO=1 JUCE_ASIO_USE_EXTERNAL_SDK=1)

# Add subdirectories BEFORE defining our target
add_subdirectory(${PTLIVE_JUCE_DIR} ${CMAKE_BINARY_DIR}/JUCE)
add_subdirectory(${PTLIVE_TRACKTION_DIR}/modules ${CMAKE_BINARY_DIR}/tracktion)

#== Application target =======================================================
# VERSION = THE place where the app version is set (bottom bar, update check, exe properties, license server log).
# Bump it here for every release; it must match "version" in the update.json uploaded to the VPS.
juce_add_gui_app(${TARGET_NAME}
    VERSION 0.1.0
    BUNDLE_ID "com.ptstudio.ptLive"
    MICROPHONE_PERMISSION_ENABLED TRUE
    STATUS_BAR_HIDDEN FALSE
    PLUGINHOST_VST3 TRUE
    NEEDS_CURL FALSE
    REQUIRES_FULL_SCREEN TRUE)

juce_generate_juce_header(${TARGET_NAME})

#== Optional: embed IBM Plex Mono ============================================
# Drop IBMPlexMono-Regular.ttf and IBMPlexMono-Bold.ttf into assets/fonts/ and re-run CMake.
# Without them the UI falls back to an installed "IBM Plex Mono", then Consolas.
set(PTLIVE_FONT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts)
if(EXISTS ${PTLIVE_FONT_DIR}/IBMPlexMono-Regular.ttf AND EXISTS ${PTLIVE_FONT_DIR}/IBMPlexMono-Bold.ttf)
    juce_add_binary_data(PtLiveAssets SOURCES
        ${PTLIVE_FONT_DIR}/IBMPlexMono-Regular.ttf
        ${PTLIVE_FONT_DIR}/IBMPlexMono-Bold.ttf)
    target_link_libraries(${TARGET_NAME} PRIVATE PtLiveAssets)
endif()

#== Application icon =========================================================
# ptlive.ico lives in the repo root. We embed it as a proper Windows RT_GROUP_ICON
# through a .rc file so it shows up in Explorer, the taskbar and Alt-Tab, and is
# used as the title-bar icon for the main window and plugin editor windows.
# (A plain VS_ICONS_RESOURCE_FILES entry only stamps the .exe on disk but is not a
#  usable icon resource, and bundling the .ico as binary data does not make it the
#  executable icon either.)
set(PTLIVE_ICON_FILE "${CMAKE_CURRENT_SOURCE_DIR}/../ptlive.ico")
if(NOT EXISTS ${PTLIVE_ICON_FILE})
    set(PTLIVE_ICON_FILE "${CMAKE_CURRENT_SOURCE_DIR}/resources/ptlive.ico")   # copy shipped with the project
endif()

if(MSVC)
    if(EXISTS ${PTLIVE_ICON_FILE})
        set_target_properties(${TARGET_NAME} PROPERTIES WIN32_EXECUTABLE TRUE)

        # Copy the icon into resources/ so ptlive.rc can reference it relatively.
        if(NOT "${PTLIVE_ICON_FILE}" STREQUAL "${CMAKE_CURRENT_SOURCE_DIR}/resources/ptlive.ico")
            configure_file(${PTLIVE_ICON_FILE}
                           ${CMAKE_CURRENT_SOURCE_DIR}/resources/ptlive.ico COPYONLY)
        endif()

        target_sources(${TARGET_NAME} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/resources/ptlive.rc)
    else()
        message(WARNING "ptlive.ico not found at ${PTLIVE_ICON_FILE}")
    endif()
endif()

#== Source files =============================================================
target_sources(${TARGET_NAME} PRIVATE
    src/MainApplication.cpp
    src/MainWindow.cpp
    src/Ktv.cpp
    src/ptLive.cpp
    src/Audio/AudioEngine.cpp
    src/Audio/TrackManager.cpp
    src/Audio/SongHistory.cpp
    src/Audio/PluginWindowPlatform.cpp
    src/UI/TransportBar.cpp
    src/UI/MasterRecorder.cpp
    src/UI/PluginScanner.cpp
    src/UI/PluginBrowserDialog.cpp
    src/UI/AudioSettingsDialog.cpp
    src/UI/VUMeter.cpp
    src/UI/MixerBoard.cpp
    src/UI/TrackRow.cpp
    src/UI/InspectorPanel.cpp
    src/UI/ControlBar.cpp
    src/Audio/LiveController.cpp
    src/Net/VpsClient.cpp
    src/Net/LicenseClient.cpp
    src/UI/LicenseDialog.cpp
    src/UI/WelcomeDialog.cpp
    src/Net/KeyFetcher.cpp
    src/Net/SongDetector.cpp
    src/Net/TabNavigator.cpp
    src/Net/UpdateChecker.cpp
    src/UI/UpdateDialog.cpp)

# License: Ed25519 token check uses TweetNaCl (public domain, https://tweetnacl.cr.yp.to/).
# Put tweetnacl.c + tweetnacl.h in third_party/. Without them the project still builds, but no key can be
# activated (LicenseClient fails closed with "no_crypto").
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/tweetnacl.c AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/tweetnacl.h)
    target_sources(${TARGET_NAME} PRIVATE third_party/tweetnacl.c)
    set_source_files_properties(third_party/tweetnacl.c PROPERTIES LANGUAGE C)
    target_compile_definitions(${TARGET_NAME} PRIVATE PTLIVE_HAS_NACL=1)
else()
    message(WARNING "third_party/tweetnacl.c / .h not found - license keys cannot be verified until they are added")
endif()

# Add include directories (project root for "include/" headers + ASIO SDK)
target_include_directories(${TARGET_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${PTLIVE_ASIO_DIR}/common)

#== Link modules =============================================================
target_link_libraries(${TARGET_NAME} PRIVATE
    tracktion::tracktion_core
    tracktion::tracktion_engine
    tracktion::tracktion_graph
    juce::juce_audio_devices
    juce::juce_audio_processors
    juce::juce_audio_utils
    juce::juce_dsp
    juce::juce_events
    juce::juce_graphics
    juce::juce_gui_basics
    juce::juce_gui_extra)

# VpsClient (WinHTTP + certificate pinning) and SongDetector (UI Automation)
if(WIN32)
    target_link_libraries(${TARGET_NAME} PRIVATE winhttp crypt32 ole32 oleaut32 uuid)
endif()

#== C++ standard ==============================================================
target_compile_features(${TARGET_NAME} PRIVATE cxx_std_20)

#== MSVC settings =============================================================
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(${TARGET_NAME} PRIVATE /W4 /utf-8)

    # P0-1: Enable AVX2 in Release - both target CPUs (i5 7th & 10th gen) support it.
    target_compile_options(${TARGET_NAME} PRIVATE
        $<$<CONFIG:Release>:/O2 /arch:AVX2 /fp:fast>)

    # Debug info + linker map for Release (no speed cost): gives ptLive.pdb / ptLive.map to locate crashes.
    target_compile_options(${TARGET_NAME} PRIVATE $<$<CONFIG:Release>:/Zi>)
    target_link_options(${TARGET_NAME} PRIVATE
        $<$<CONFIG:Release>:/DEBUG /OPT:REF /OPT:ICF /MAP>)
endif()
