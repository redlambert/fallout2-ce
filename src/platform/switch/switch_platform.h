#ifndef FALLOUT_PLATFORM_SWITCH_SWITCH_PLATFORM_H_
#define FALLOUT_PLATFORM_SWITCH_SWITCH_PLATFORM_H_

#ifdef __SWITCH__

#include "dinput.h"
#include "svga.h"

namespace fallout {

// Mounts the SD card, picks the game folder and changes into it.
//
// Fallout 2 lives in `sdmc:/switch/fallout2/` (or `sdmc:/fallout2/`).
// When launched from an NRO whose file name mentions "ettu" (for example
// `fallout1-ettu.nro`), the game starts inside the `Fallout1in2/` subfolder
// instead, which is where Fallout et tu is installed.
void switchPlatformInit(int argc, char** argv);

// Folder that holds Fallout 2's `master.dat`, with trailing slash.
const char* switchGetBaseDir();

// Replaces screen settings with values that suit the Switch display.
// Defaults can be changed in `fallout2_nx.ini` in the base folder.
void switchApplyScreenOverrides(int* width, int* height, int* scale, WindowMode* mode);

// Controller to keyboard mapping. Pushes SDL key events, so they flow
// through the same path as a physical keyboard (including sfall hotkeys).
void switchInputInit();
void switchInputPoll(bool textInputActive);

// Left stick moves the cursor, ZL/ZR are left/right mouse buttons.
void switchMouseApply(MouseData* mouseData);

// Performance logging. When PERF_LOG=1 in fallout2_nx.ini, a summary line is
// appended to perf.log in the base folder every 5 seconds.
void switchPerfRecordPresent(unsigned long long presentStartCounter);
void switchPerfCountFileOpen();

} // namespace fallout

#endif /* __SWITCH__ */

#endif /* FALLOUT_PLATFORM_SWITCH_SWITCH_PLATFORM_H_ */
