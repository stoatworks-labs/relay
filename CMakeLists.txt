cmake_minimum_required(VERSION 3.15)

project(Relay
    VERSION 0.1.0
    LANGUAGES CXX
)

# ---------------------------------------------------------------------------
# Universal by default so one bundle loads in both the Apple Silicon and the
# Intel build of Resolume. Override with -DCMAKE_OSX_ARCHITECTURES=arm64 for a
# faster dev build.
#
# This must sit immediately after project() and before any target exists. CMake
# latches the architecture list when the first target is created; setting it
# later is silently ignored and you get an arm64-only binary that the build log
# calls a success. Verify with `lipo`, never with the log.
# ---------------------------------------------------------------------------
if(APPLE AND NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Apple deprecated the whole OpenGL API in 10.14 and FFGL's plugin interface is
# still OpenGL, so every SDK call warns. There is no alternative API to move to
# here, so silencing it is not ignoring a problem.
if(APPLE)
    add_compile_definitions(GL_SILENCE_DEPRECATION)
endif()

if(MSVC)
    add_compile_options(/utf-8)
endif()

# windows.h defines min and max as macros, which turns every std::min into
# `std::(...)` and fails with an error naming neither min nor windows.h.
if(WIN32)
    add_compile_definitions(NOMINMAX)
endif()

option(RELAY_BUILD_TOOLS "Build the offline render and measurement harness" ON)

# The FFGL plugin, the FFGL SDK submodule and GLEW are one dependency group, and
# the OpenFX plugin belongs to none of it. Turning this OFF builds the OFX plugin
# alone, with no GL loader anywhere in the configure -- which is what the Linux
# job does, because the FFGL SDK has no Linux path and GLEW would otherwise have
# to be installed to build something that never calls it.
option(RELAY_BUILD_FFGL "Build the FFGL plugin (needs the FFGL SDK and, off macOS, GLEW)" ON)

# ---------------------------------------------------------------------------
# The relay with no GL in it at all: the parameter conversions, the raster,
# the coil, the bounce schedule, the re-lock and the crosstalk filter (the
# arithmetic both builds share); the frame plan the pass is handed; the
# OpenFX build's reading of a Transition curve; and the pass transcribed to
# C++.
#
# A separate library from the FFGL core because the OpenFX plugin renders on
# the CPU and the Linux job has no GL loader configured. Splitting it is what
# lets the OFX build link the SAME arithmetic rather than a second
# transcription of it -- and lets the harness put the C++ pass beside the GLSL
# one.
# ---------------------------------------------------------------------------
add_library(relay_dsp OBJECT
    source/Controls.h
    source/Controls.cpp
    source/Raster.h
    source/Raster.cpp
    source/Model.h
    source/Model.cpp
    source/Frame.h
    source/Frame.cpp
    source/Pass.h
    source/Pass.cpp
    source/Transition.h
    source/Transition.cpp
)
target_include_directories(relay_dsp PUBLIC source)
set_target_properties(relay_dsp PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(RELAY_BUILD_FFGL)

# ---------------------------------------------------------------------------
# The Resolume FFGL SDK, as a git submodule pinned to b1afaf9 -- the rev the
# rest of the fleet pins.
# ---------------------------------------------------------------------------
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/ffgl/CMakeLists.txt")
    message(FATAL_ERROR
        "FFGL SDK not found. Run: git submodule update --init --recursive")
endif()

set(FFGL_BUILD_EXAMPLE_PLUGINS OFF CACHE BOOL "" FORCE)
add_subdirectory(external/ffgl EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------------------
# Everything the plugin is, as an OBJECT library so both the bundle and the
# offline harness are built from exactly the same code.
#
# OBJECT and not STATIC on purpose. `CFFGLPluginInfo` registers itself from a
# file-scope constructor in Relay.cpp and nothing ever references that object
# by name, so in an archive the linker is entitled to drop the whole
# translation unit -- and the result is a bundle that loads, exports plugMain,
# and reports that it contains no plugins.
# ---------------------------------------------------------------------------
add_library(relay_core OBJECT
    source/StoatworksAbout.h
    source/StoatworksAboutLinks.h
    source/StoatworksAboutParams.h
    source/Diag.h
    source/Diag.cpp
    source/Timing.h
    source/Timing.cpp
    source/Shaders.h
    source/Shaders.cpp
    source/Relay.h
    source/Relay.cpp
)

target_include_directories(relay_core PUBLIC source)
target_link_libraries(relay_core PUBLIC ffgl::sdk relay_dsp)
target_compile_definitions(relay_core PUBLIC RELAY_VERSION="${PROJECT_VERSION}")
set_target_properties(relay_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

# On Windows/Linux the SDK headers pull in <GL/glew.h>. The SDK links GLEW
# privately, so consumers need it too -- for the include path and for the
# GLEW_STATIC define the static vcpkg triplet propagates. macOS uses the system
# OpenGL framework and needs none of this. GLEW arrives through vcpkg.json.
if(NOT APPLE)
    find_package(GLEW REQUIRED)
    target_link_libraries(relay_core PUBLIC GLEW::GLEW)
endif()

# ---------------------------------------------------------------------------
# The plugin. FFGL resolves one plugMain per binary.
# ---------------------------------------------------------------------------
function(relay_add_plugin target entry bundle_name identifier)
    add_library(${target} MODULE ${entry})
    # relay_dsp is named again here, and on every other final target, because
    # an OBJECT library's objects do NOT travel transitively through a second
    # OBJECT library: relay_core linking it gives this target the include
    # paths and nothing to link.
    target_link_libraries(${target} PRIVATE relay_core relay_dsp)

    if(APPLE)
        set_target_properties(${target} PROPERTIES
            BUNDLE TRUE
            BUNDLE_EXTENSION "bundle"
            OUTPUT_NAME "${bundle_name}"
            MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Info.plist.in"
            MACOSX_BUNDLE_BUNDLE_NAME "${bundle_name}"
            MACOSX_BUNDLE_GUI_IDENTIFIER "${identifier}"
            MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        )
        # FFGL hosts resolve plugMain by symbol name, so keep default visibility.
        target_link_libraries(${target} PRIVATE
            "-framework OpenGL"
            "-framework Carbon"
            "-framework AppKit"
        )
    else()
        set_target_properties(${target} PROPERTIES OUTPUT_NAME "${bundle_name}")
    endif()
endfunction()

relay_add_plugin(Relay source/PluginEntry.cpp
    "Relay" "com.stoatworks.ffgl.relay")

# `cmake --install build` drops the bundle where Arena reads it. Avenue reads
# ~/Documents/Resolume Avenue/Extra Effects instead; pass --prefix for that.
#
# A mixer goes in Extra Effects like every other FFGL plugin. This said
# "Extra Mixers" until v0.1.0, inherited from genlock's assumption that
# Resolume keeps mixers apart; it does not. Measured on genlock in Resolume
# Arena 7.27.1 (2026-09-23): Arena scans Extra Effects, loads the mixer from
# there and offers it as a layer Blend Mode, and its binary names no "Extra
# Mixers" anywhere. An install to Extra Mixers would install nothing Arena sees.
if(APPLE AND CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "$ENV{HOME}/Documents/Resolume Arena/Extra Effects"
        CACHE PATH "" FORCE)
endif()
install(TARGETS Relay
    LIBRARY DESTINATION "."
    BUNDLE  DESTINATION "."
)

# ---------------------------------------------------------------------------
# The harness: the real plugin class in a real GL context, with no host.
#
# macOS only, because it stands up a CGL context directly. It feeds TWO
# inputs, because this is a mixer (the fleet's third) -- see
# tools/rltest/main.cpp.
# ---------------------------------------------------------------------------
if(RELAY_BUILD_TOOLS AND APPLE)
    enable_testing()

    add_executable(rltest tools/rltest/main.cpp)
    # zlib ships with the OS, which is why the PNG writer is fifty lines
    # rather than a vendored dependency.
    target_link_libraries(rltest PRIVATE relay_core relay_dsp z
        "-framework OpenGL"
        "-framework AppKit"
    )

    add_test(NAME names      COMMAND rltest --names)
    add_test(NAME mixer      COMMAND rltest --mixer)
    add_test(NAME ends       COMMAND rltest --ends)
    add_test(NAME hysteresis COMMAND rltest --hysteresis)
    add_test(NAME bounce     COMMAND rltest --bounce)
    add_test(NAME vi         COMMAND rltest --vi)
    add_test(NAME relock     COMMAND rltest --relock)
    add_test(NAME crosstalk  COMMAND rltest --crosstalk)
    add_test(NAME mutation   COMMAND rltest --mutation)
    add_test(NAME transition COMMAND rltest --transition)
endif()

endif()  # RELAY_BUILD_FFGL

# ---------------------------------------------------------------------------
# The OpenFX build, for Resolve, Vegas and the other hosts with a Transition
# context. CPU render against the vendored OFX SDK subset under
# external/openfx (C headers + the official C++ Support library, BSD-3).
# relay_dsp links straight in, so the coil, the bounce schedule, the re-lock,
# the crosstalk filter and the parameter conversions are the same code as the
# FFGL build; only the per-pixel pass is written twice (Shaders.cpp and
# Pass.cpp, marked `//= mirrored`).
# ---------------------------------------------------------------------------
option(BUILD_OFX "Build the OpenFX plugin" ON)

if(BUILD_OFX)
    set(OFX_SDK "${CMAKE_CURRENT_SOURCE_DIR}/external/openfx")

    add_library(RelayOFX MODULE
        source/ofx/RelayOFX.cpp
        ${OFX_SDK}/Support/Library/ofxsCore.cpp
        ${OFX_SDK}/Support/Library/ofxsImageEffect.cpp
        ${OFX_SDK}/Support/Library/ofxsInteract.cpp
        ${OFX_SDK}/Support/Library/ofxsLog.cpp
        ${OFX_SDK}/Support/Library/ofxsMultiThread.cpp
        ${OFX_SDK}/Support/Library/ofxsParams.cpp
        ${OFX_SDK}/Support/Library/ofxsProperty.cpp
        ${OFX_SDK}/Support/Library/ofxsPropertyValidation.cpp
    )
    # PRIVATE on an OBJECT library gives both the objects and the usage
    # requirements. `$<TARGET_OBJECTS:relay_dsp>` would give the objects alone
    # -- no include directories, no compile definitions.
    target_link_libraries(RelayOFX PRIVATE relay_dsp)
    target_include_directories(RelayOFX PRIVATE
        source
        "${OFX_SDK}/include"
        "${OFX_SDK}/Support/include"
    )
    target_compile_definitions(RelayOFX PRIVATE
        RELAY_VERSION="${PROJECT_VERSION}"
        PLUGIN_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
        PLUGIN_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    )

    # Threads, explicitly. The OFX Support library's multi-thread suite calls
    # pthread_create, and on Rocky 8 -- glibc 2.28, the floor Resolve supports --
    # that still lives in libpthread; libpthread was only folded into libc in
    # glibc 2.34. Leave it out and the plugin compiles, links, passes a symbol
    # check and a glibc-version check, and then fails at dlopen with "undefined
    # symbol: pthread_create". macOS and Windows never show it.
    find_package(Threads REQUIRED)
    target_link_libraries(RelayOFX PRIVATE Threads::Threads)

    set(OFX_BUNDLE_DIR "${CMAKE_BINARY_DIR}/Relay.ofx.bundle")
    set_target_properties(RelayOFX PROPERTIES
        PREFIX ""
        SUFFIX ".ofx"
        OUTPUT_NAME "Relay"
        LIBRARY_OUTPUT_DIRECTORY "${OFX_BUNDLE_DIR}/Contents/MacOS"
    )
    if(APPLE)
        set(OFX_BUNDLE_IDENTIFIER "com.stoatworks.relay.ofx")
        configure_file(cmake/InfoOFX.plist.in
            "${OFX_BUNDLE_DIR}/Contents/Info.plist" @ONLY)
    elseif(WIN32)
        set_target_properties(RelayOFX PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY "${OFX_BUNDLE_DIR}/Contents/Win64"
            RUNTIME_OUTPUT_DIRECTORY "${OFX_BUNDLE_DIR}/Contents/Win64"
            LIBRARY_OUTPUT_DIRECTORY_RELEASE "${OFX_BUNDLE_DIR}/Contents/Win64"
            RUNTIME_OUTPUT_DIRECTORY_RELEASE "${OFX_BUNDLE_DIR}/Contents/Win64"
            # MSVC's .exp/.lib for this target; its default directory collides
            # with the FFGL target's, which shares the output base name, and a
            # parallel build then races on the file (LNK1104).
            ARCHIVE_OUTPUT_DIRECTORY "${OFX_BUNDLE_DIR}/importlib"
            ARCHIVE_OUTPUT_DIRECTORY_RELEASE "${OFX_BUNDLE_DIR}/importlib")
    else()
        set_target_properties(RelayOFX PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY "${OFX_BUNDLE_DIR}/Contents/Linux-x86-64")
    endif()
endif()
