cmake_minimum_required(VERSION 3.16)
project(EclipseConsole CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# The app icon is a compiled resource, so the RC language has to be on. Both
# binaries get it: the CLI shows up in Explorer and Alt-Tab too, and an icon on
# one but not the other looks like a packaging mistake.
enable_language(RC)

# app.rc references icon.ico, and nothing scans a .rc for its dependencies. So
# editing the artwork alone left app.rc untouched, make saw nothing to do, and
# the build "succeeded" while still embedding the PREVIOUS icon -- silently,
# because the resource is the same byte length whatever the pixels are. Caught
# by hashing the embedded 256px layer against the source.
set_property(SOURCE src/res/app.rc PROPERTY OBJECT_DEPENDS
             "${CMAKE_CURRENT_SOURCE_DIR}/src/res/icon.ico")

# Use the SDK checked in under third_party rather than letting webview fetch its
# own, so the build stays offline and the version is pinned with the source.
set(FETCHCONTENT_SOURCE_DIR_MICROSOFT_WEB_WEBVIEW2
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/webview2/extracted"
    CACHE PATH "Pre-extracted WebView2 SDK")
set(MSWebView2_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/webview2/extracted"
    CACHE PATH "Pre-extracted WebView2 SDK")

# webview 0.12 ships its own WebView2 loader, so nothing links against
# WebView2Loader.dll and the exe stays self-contained under static linking.
FetchContent_Declare(
  webview
  GIT_REPOSITORY https://github.com/webview/webview.git
  GIT_TAG        0.12.0
)
FetchContent_MakeAvailable(webview)

# Stop the library showing the window before there is anything in it. See the
# script for why this runs here rather than as a FetchContent PATCH_COMMAND.
set(WEBVIEW_HEADER "${webview_SOURCE_DIR}/core/include/webview/webview.h")
include(${CMAKE_CURRENT_SOURCE_DIR}/tools/patch_webview.cmake)

add_executable(eclipse_console WIN32
    src/core/eclipse_console.cpp
    src/core/offsets/offsets.cpp
    src/core/memory/stubs.cpp
    src/core/memory/read.cpp
    src/core/offsets/health.cpp
    src/core/offsets/conf.cpp
    src/core/cache.cpp
    src/core/crash.cpp
    src/core/features/runtime.cpp
    src/core/features/window.cpp
    src/core/features/config.cpp
    src/core/features/world.cpp
    src/core/features/input.cpp
    src/core/features/net.cpp
    src/core/memory/write.cpp
    src/core/snippet_build.cpp
    src/ui/eclipse_ui.cpp
    src/ui/overlay.cpp
    src/ui/dpi.cpp
    src/ui/log.cpp
    src/ui/snipwin.cpp
    src/ui/client/attach.cpp
    src/ui/client/launch.cpp
    src/ui/client/offsets_fetch.cpp
    src/ui/client/placename.cpp
    src/ui/snippets/bundle.cpp
    src/ui/snippets/tree.cpp
    src/ui/snippets/trash.cpp
    src/ui/snippets/watch.cpp
    src/res/app.rc
)

# fx.h switches between dllexport and dllimport on this. The host defines it;
# snippets never do, so they import.
target_compile_definitions(eclipse_console PRIVATE FX_HOST ECLIPSE_NO_AUTO_SHOW)
target_include_directories(eclipse_console PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/memory
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/offsets
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/features
    # src/ui + its subdirectories keep bare #include lines resolving across
    # the ui/ split, same as the src/core/* entries above do for core.
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/client
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/snippets)
# comdlg32: GetSaveFileName/GetOpenFileName for the bundle export/import.
target_link_libraries(eclipse_console PRIVATE webview::core dwmapi winhttp comdlg32)

# -g ONLY, deliberately: no optimisation flags are changed, so the code
# generated is byte-for-byte what it was. Without it the crash handler's
# backtrace resolves to "eclipse_console.exe+0x1df33" and addr2line answers
# "??" -- a module and an offset, which is barely better than the WER bucket
# that made the heap-corruption bug undiagnosable for weeks. With it:
#     addr2line -e build/eclipse_console.exe -f -C 0x1df33
# names the function and the line. Debug info costs file size and nothing at
# run time; strip it at packaging time if the installer ever needs to be small.
target_compile_options(eclipse_console PRIVATE -g)

# Static MinGW runtime, so the exe imports nothing outside Windows itself.
# Without this it depends on libwinpthread-1.dll, which then has to be copied
# beside it or found on PATH -- one more thing to forget when handing the app
# to someone, and it fails at launch with a dialog that names a DLL rather
# than a cause.
target_link_options(eclipse_console PRIVATE -static -static-libgcc -static-libstdc++)
# eclipse_cli gets the same, but not here -- it is not declared until further
# down, and target_link_options on an undefined target is a hard error.

set_target_properties(eclipse_console PROPERTIES
    OUTPUT_NAME "eclipse_console"          # the import library records this name
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

if(MINGW)
    target_link_options(eclipse_console PRIVATE
        -mwindows
        -static-libgcc -static-libstdc++
        # THE -rdynamic REPLACEMENT.
        # On ELF the host was built -rdynamic and a dlopen'd .so resolved fx_*
        # straight back into it. Windows has no such thing: a DLL binds to an
        # EXE only through an import library. This emits one, and run_snippet()
        # compiles every snippet against it. Without this the port does not
        # work at all -- snippets fail to link.
        "-Wl,--out-implib,${CMAKE_BINARY_DIR}/libeclipse_console.a"
    )
endif()

# ---------------------------------------------------------------------------
# The CLI, as a CONSOLE-subsystem binary.
#
# An executable has exactly one subsystem and the two modes want opposite ones.
# The GUI build must not flash a console window when the app is launched, so it
# can only borrow one via AttachConsole -- and then the shell does not wait for
# it, output interleaves with the prompt, and Ctrl+C is delivered to the shell
# instead of to us. A console-subsystem binary gets all three for free.
#
# It shares eclipse_console.cpp, so it exports the same fx_* and emits its OWN
# import library: a snippet is loaded into whichever process is running, and must
# bind back into that one. build_and_run() picks the -l from the module name.
add_executable(eclipse_cli
    src/core/eclipse_console.cpp
    src/core/offsets/offsets.cpp
    src/core/memory/stubs.cpp
    src/core/memory/read.cpp
    src/core/offsets/health.cpp
    src/core/offsets/conf.cpp
    src/core/cache.cpp
    src/core/crash.cpp
    src/core/features/runtime.cpp
    src/core/features/window.cpp
    src/core/features/config.cpp
    src/core/features/world.cpp
    src/core/features/input.cpp
    src/core/features/net.cpp
    src/core/memory/write.cpp
    src/core/snippet_build.cpp
    src/cli/eclipse_cli.cpp
    src/res/app.rc
)
target_compile_definitions(eclipse_cli PRIVATE FX_HOST)
target_include_directories(eclipse_cli PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/memory
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/offsets
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/features)

# winhttp: core/features/net.cpp is in BOTH targets, so the CLI needs it too --
# fx_http_get / fx_avatar_url are not GUI features and a snippet run with
# --run must find the same fx_* set the console exports.
target_link_libraries(eclipse_cli PRIVATE winhttp)
target_compile_options(eclipse_cli PRIVATE -g)   # see the console's, above

set_target_properties(eclipse_cli PROPERTIES
    OUTPUT_NAME "eclipse_cli"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

if(MINGW)
    target_link_options(eclipse_cli PRIVATE
        # No -mwindows here: that is what makes the difference.
        # -static pulls in libwinpthread too, so the binary imports nothing
        # outside Windows and can be copied to a machine with no MinGW.
        -static -static-libgcc -static-libstdc++
        "-Wl,--out-implib,${CMAKE_BINARY_DIR}/libeclipse_cli.a"
    )
endif()

# Everything the exe reads at runtime lives beside it. All of it is meant to be
# editable without a rebuild: the conf (a Roblox version bump is a regen) and
# the web/ UI. Snippets are NOT here -- they live in %APPDATA%\EclipseConsole\
# snippets, per user, so nothing the build does can touch them.
#
# Standalone ALL target rather than a POST_BUILD on the exe: POST_BUILD only
# fires when the target relinks, so editing a .css or .html would leave the
# staged copy stale while the build reports success.
add_custom_target(eclipse_assets ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/conf"
    # NOT a plain copy: the staged conf is owned by the RUNNING APP, which
    # re-fetches it on every Roblox update. Overwriting it here silently undid
    # that fetch on each rebuild and left the app refusing to run on a version
    # mismatch. See tools/stage_conf.cmake for how to force the repo copy out.
    COMMAND ${CMAKE_COMMAND}
            -D "SRC=${CMAKE_CURRENT_SOURCE_DIR}/conf/offsets_windows.conf"
            -D "DST=${CMAKE_BINARY_DIR}/conf/offsets_windows.conf"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tools/stage_conf.cmake"
    # Snippets #include "fx.h", so it has to sit next to the exe too.
    COMMAND ${CMAKE_COMMAND} -E copy
            "${CMAKE_CURRENT_SOURCE_DIR}/src/fx.h"
            "${CMAKE_BINARY_DIR}/fx.h"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/web"
            "${CMAKE_BINARY_DIR}/web"
    COMMENT "staging conf/, fx.h and web/ next to the exe"
    VERBATIM
)
