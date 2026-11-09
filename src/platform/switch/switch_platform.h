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

// True when presenting should wait for the screen refresh (VSYNC in
// fallout2_nx.ini, on by default).
bool switchWantsVsync();

// Ways of handing frames to the graphics driver, set in fallout2_nx.ini:
// RENDER_TEXTURES (1 or 2), RENDER_UPLOAD (0 = update, 1 = lock),
// RENDER_BATCHING (1 or 0), RENDER_RESET_SECONDS (0 = off).
int switchRenderTextureCount();
bool switchRenderUsesLock();
bool switchRenderBatching();
int switchRenderResetSeconds();

// Replaces screen settings with values that suit the Switch display.
// Defaults can be changed in `fallout2_nx.ini` in the base folder.
void switchApplyScreenOverrides(int* width, int* height, int* scale, WindowMode* mode);

// Controller to keyboard mapping. Pushes SDL key events, so they flow
// through the same path as a physical keyboard (including sfall hotkeys).
void switchInputInit();
void switchInputPoll(bool textInputActive);

// Left stick moves the cursor, ZL/ZR are left/right mouse buttons.
void switchMouseApply(MouseData* mouseData);

// Performance logging. When PERF_LOG=1 in fallout2_nx.ini, a summary is
// appended to perf.log in the base folder every 5 seconds.
void switchPerfRecordPresent(unsigned long long presentStartCounter);
void switchPerfRecordFileOpen(const char* path, unsigned long long startCounter, bool found);
void switchPerfRecordDirectoryScan(const char* path, unsigned long long startCounter);
void switchPerfCountDirectoryCacheClear();
void switchPerfRecordFileOpenError(const char* path, int error);
void switchPerfCountWindowEvent(int windowEvent);
void switchPerfCountRendererRebuild();

// Called when part of the screen is redrawn, so the log can tell frames
// where the picture changed from frames that only repeat the last one.
void switchPerfMarkScreenUpdate();

// Records which line of code asked for a frame to be presented.
void switchPerfRecordPresentSource(const char* file, int line);

// Counts palette changes (lighting, color cycling), each of which redraws
// the whole screen.
void switchPerfRecordPaletteChange(unsigned long long startCounter);

// Records a failed texture upload or draw call, with SDL's error message.
void switchPerfRecordRenderError(const char* step, const char* error);

// Hashes the finished frame to count frames whose picture really changed.
void switchPerfRecordPictureHash(const void* pixels, int pitch, int height);

// Parts of the frame that get timed separately.
enum class SwitchPerfZoneId {
    Input,
    WindowRefresh,
    MapRender,
    Scripts,
    GlobalScripts,
    Programs,
    Sound,
    Count,
};

// Times the enclosing block. Nested zones of the same kind only count once.
class SwitchPerfZone {
public:
    explicit SwitchPerfZone(SwitchPerfZoneId id);
    ~SwitchPerfZone();

private:
    int _id;
    unsigned long long _start;
    bool _outermost;
};

} // namespace fallout

#endif /* __SWITCH__ */

#endif /* FALLOUT_PLATFORM_SWITCH_SWITCH_PLATFORM_H_ */
