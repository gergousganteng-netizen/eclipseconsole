# Patch webview 0.12's header so it does not show the window itself.
#
# webview's constructor calls ShowWindow BEFORE embed(), so the window appears
# blank and stays blank for the whole of WebView2's startup -- a visible flash of
# empty chrome before the UI arrives. Nothing the app does after the constructor
# returns can help, because by then the flash has already happened.
#
# So the show is put behind a macro and the app does it itself once the page
# reports READY. Everything else about the library is untouched.
#
# Run at configure time rather than as a FetchContent PATCH_COMMAND: that only
# fires on first populate, so an already-populated build directory would silently
# keep the unpatched header. This is idempotent and checks every configure.
#
# Bracket arguments and a whitespace-tolerant regex are both deliberate. A plain
# quoted needle has to escape every semicolon, and matches only one exact spacing
# and one line ending -- which is how the first version of this failed.

if(NOT EXISTS "${WEBVIEW_HEADER}")
  message(FATAL_ERROR "patch_webview: no header at ${WEBVIEW_HEADER}")
endif()

file(READ "${WEBVIEW_HEADER}" src)

if(src MATCHES "ECLIPSE_NO_AUTO_SHOW")
  return()   # already patched
endif()

# One line, not the whole block. CMake's regex does not interpret \n or \t
# inside a character class, so a multi-line pattern silently matches nothing --
# and a quoted needle would need every semicolon escaped. A single line in a
# bracket argument sidesteps both.
#
# The line is unique: the other ShowWindow in this header takes m_widget.
# UpdateWindow and SetFocus are left alone deliberately -- both are harmless on
# a window that is still hidden.
set(needle [[ShowWindow(m_window, SW_SHOW);]])

string(FIND "${src}" "${needle}" found)
if(found EQUAL -1)
  message(FATAL_ERROR
    "patch_webview: could not find '${needle}' in ${WEBVIEW_HEADER}. The pinned "
    "webview version probably changed; re-check the constructor before editing "
    "this script.")
endif()

set(replacement [[
#ifndef ECLIPSE_NO_AUTO_SHOW  /* eclipse: tools/patch_webview.cmake */
      ShowWindow(m_window, SW_SHOW);
#endif]])

string(REPLACE "${needle}" "${replacement}" src "${src}")
file(WRITE "${WEBVIEW_HEADER}" "${src}")
message(STATUS "patched webview: the app shows the window, not the library")
