// launch.h -- launching Roblox. Split out of eclipse_ui.cpp; the how and
// why live with the code in launch.cpp.
#pragma once

struct Launcher { const char* label; const char* path; };

Launcher detect_launcher();
void launch_roblox();
