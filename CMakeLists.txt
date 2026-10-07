# GPL-3.0-or-later — Copyright (c) 2026 Luciano Dato — plugprobe
cmake_minimum_required(VERSION 3.22)
# Universal macOS binary (arm64+x86_64). Set before project() so every target
# inherits it; harmless elsewhere (only consumed for Apple targets).
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64")
project(plugprobe VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
if(UNIX AND NOT APPLE)
    # Static language runtimes only; desktop syslibs (X11/asound/fontconfig)
    # stay dynamic per workspace policy (declare in .deb metadata).
    add_link_options(-static-libgcc -static-libstdc++ -Wl,--exclude-libs,ALL)
endif()

include(FetchContent)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../JUCE/CMakeLists.txt")
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../JUCE" "${CMAKE_CURRENT_BINARY_DIR}/juce_build")
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/subprojects/JUCE/CMakeLists.txt")
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/subprojects/JUCE" "${CMAKE_CURRENT_BINARY_DIR}/juce_build")
else()
    FetchContent_Declare(JUCE GIT_REPOSITORY https://github.com/juce-framework/JUCE.git GIT_TAG 8.0.15)
    FetchContent_MakeAvailable(JUCE)
endif()

juce_add_console_app(plugprobe PRODUCT_NAME "plugprobe")
if(APPLE)
    # Explicit per-target slices: CMAKE_OSX_ARCHITECTURES alone produced a
    # thin arm64 binary on one CI toolchain despite being in the cache.
    set_target_properties(plugprobe PROPERTIES OSX_ARCHITECTURES "arm64;x86_64")
endif()

add_subdirectory(example/lowpass)
target_sources(plugprobe PRIVATE src/main.cpp src/core.cpp src/gui.cpp
    src/cmd_scan.cpp src/cmd_inspect.cpp src/cmd_snapshot.cpp src/cmd_act.cpp
    src/cmd_render.cpp src/cmd_compare.cpp src/cmd_session.cpp src/session.cpp
    src/cmd_manual.cpp)
if(APPLE)
    target_sources(plugprobe PRIVATE src/plugprobe_os_mac.mm)
elseif(WIN32)
    target_sources(plugprobe PRIVATE src/plugprobe_os_win.cpp)
else()
    target_sources(plugprobe PRIVATE src/plugprobe_os_linux.cpp)
endif()
target_compile_features(plugprobe PRIVATE cxx_std_20)
target_compile_definitions(plugprobe PRIVATE
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_LV2=1
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    PLUGPROBE_VERSION="${PROJECT_VERSION}")
if(APPLE)
    target_compile_definitions(plugprobe PRIVATE JUCE_PLUGINHOST_AU=1)
    # console apps don't get JUCE's GUI-app framework links; AU hosting needs them
    target_link_libraries(plugprobe PRIVATE
        "-framework AudioUnit" "-framework AudioToolbox" "-framework CoreAudio"
        "-framework CoreAudioKit" "-framework Cocoa" "-framework ApplicationServices"
        "-framework AVFoundation" "-framework CoreMedia")
endif()
if(WIN32)
    target_link_libraries(plugprobe PRIVATE UIAutomationCore gdiplus)
endif()
if(UNIX AND NOT APPLE)
    find_package(X11 REQUIRED)
    find_library(XTEST_LIB Xtst REQUIRED)
    target_include_directories(plugprobe PRIVATE ${X11_INCLUDE_DIR})
    target_link_libraries(plugprobe PRIVATE ${X11_X11_LIB} ${XTEST_LIB})
endif()
target_link_libraries(plugprobe PRIVATE
    juce::juce_audio_processors
    juce::juce_audio_formats
    juce::juce_audio_basics
    juce::juce_dsp
    juce::juce_core)

include(GNUInstallDirs)
install(TARGETS plugprobe RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(FILES man/plugprobe.1 DESTINATION "${CMAKE_INSTALL_MANDIR}/man1")
install(FILES completions/plugprobe.bash
        DESTINATION "${CMAKE_INSTALL_DATADIR}/bash-completion/completions"
        RENAME plugprobe)
install(FILES completions/plugprobe.zsh
        DESTINATION "${CMAKE_INSTALL_DATADIR}/zsh/site-functions"
        RENAME _plugprobe)
install(FILES completions/plugprobe.fish
        DESTINATION "${CMAKE_INSTALL_DATADIR}/fish/vendor_completions.d")
