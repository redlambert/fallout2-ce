#ifdef __SWITCH__

#include "platform/switch/switch_platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>

#include <algorithm>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <switch.h>

#include <SDL.h>

#include "config.h"
#include "debug.h"
#include "mouse.h"

namespace fallout {

// Both NROs share one binary. The Et tu one is told apart by its file name.
static const char* const kEtTuNroMarkers[] = {
    "ettu",
    "et-tu",
    "et_tu",
    "fallout1",
};

static const char* const kBaseDirCandidates[] = {
    "sdmc:/switch/fallout2/",
    "sdmc:/fallout2/",
};

static const char* const kEtTuSubdir = "Fallout1in2/";
static const char* const kSwitchConfigFileName = "fallout2_nx.ini";

// Default layout matches the previous Switch release: 854x480 game
// resolution, drawn at 2x.
static const int kDefaultWidth = 1708;
static const int kDefaultHeight = 960;
static const int kDefaultScale = 2;

static const int kLeftStickDeadZone = 8000;
static const int kRightStickDeadZone = 8000;

static char gSwitchBaseDir[256] = "sdmc:/switch/fallout2/";

// Separate pad states for keyboard and mouse. Each one is polled from a
// different place, and sharing one would make them steal each other's
// "button just pressed" edges.
static PadState gKeyboardPad;
static PadState gMousePad;
static bool gSwitchInputInitialized = false;

static double gCursorSpeedup = 1.0;

// Options read from fallout2_nx.ini at startup.
static bool gCpuBoostEnabled = true;
static int gCpuBoostMode = 1;
static bool gPerfLogEnabled = true;
static bool gVsyncEnabled = true;

static bool fileExists(const char* dir, const char* name)
{
    char path[512];
    snprintf(path, sizeof(path), "%s%s", dir, name);

    FILE* stream = fopen(path, "rb");
    if (stream == nullptr) {
        return false;
    }

    fclose(stream);
    return true;
}

static bool dirExists(const char* dir)
{
    return access(dir, F_OK) == 0;
}

// Case-insensitive substring search (strcasestr isn't always available).
static bool containsIgnoreCase(const char* haystack, const char* needle)
{
    size_t needleLength = strlen(needle);
    for (const char* start = haystack; *start != '\0'; start++) {
        size_t index = 0;
        while (index < needleLength
            && start[index] != '\0'
            && tolower(static_cast<unsigned char>(start[index])) == tolower(static_cast<unsigned char>(needle[index]))) {
            index++;
        }

        if (index == needleLength) {
            return true;
        }
    }

    return false;
}

static bool launchedAsEtTu(int argc, char** argv)
{
    if (argc < 1 || argv == nullptr || argv[0] == nullptr) {
        return false;
    }

    const char* name = strrchr(argv[0], '/');
    name = name != nullptr ? name + 1 : argv[0];

    for (const char* marker : kEtTuNroMarkers) {
        if (containsIgnoreCase(name, marker)) {
            return true;
        }
    }

    return false;
}

// Reads a number from fallout2_nx.ini without the game's config system,
// which isn't ready this early in startup.
static int readIniInt(const char* path, const char* key, int defaultValue)
{
    FILE* stream = fopen(path, "rt");
    if (stream == nullptr) {
        return defaultValue;
    }

    int value = defaultValue;
    size_t keyLength = strlen(key);
    char line[256];

    while (fgets(line, sizeof(line), stream) != nullptr) {
        char* ch = line;
        while (*ch == ' ' || *ch == '\t') {
            ch++;
        }

        if (SDL_strncasecmp(ch, key, keyLength) != 0) {
            continue;
        }

        ch += keyLength;
        while (*ch == ' ' || *ch == '\t') {
            ch++;
        }

        if (*ch != '=') {
            continue;
        }

        ch++;
        while (*ch == ' ' || *ch == '\t') {
            ch++;
        }

        if (*ch >= '0' && *ch <= '9') {
            value = atoi(ch);
        }
    }

    fclose(stream);
    return value;
}

static unsigned int gCpuBoostFailures = 0;
static unsigned int gCpuBoostLastError = 0;

static void restoreCpuClock()
{
    appletSetCpuBoostMode(static_cast<ApmCpuBoostMode>(0));
}

// Asks for the higher CPU clock. Opening a system screen (such as the
// on-screen keyboard) resets it, so this is repeated every few seconds.
//
// Mode 1 ("fast load") is the only boost available to games: CPU at
// 1785 MHz, graphics chip lowered to 76.8 MHz.
static void applyCpuBoost()
{
    Result rc = appletSetCpuBoostMode(static_cast<ApmCpuBoostMode>(gCpuBoostMode));
    if (R_FAILED(rc)) {
        gCpuBoostFailures++;
        gCpuBoostLastError = rc;
    }
}

void switchPlatformInit(int argc, char** argv)
{
    SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "0");
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");

    for (const char* candidate : kBaseDirCandidates) {
        if (fileExists(candidate, "master.dat")) {
            snprintf(gSwitchBaseDir, sizeof(gSwitchBaseDir), "%s", candidate);
            break;
        }
    }

    char gameDir[512];
    snprintf(gameDir, sizeof(gameDir), "%s", gSwitchBaseDir);

    if (launchedAsEtTu(argc, argv)) {
        char etTuDir[512];
        snprintf(etTuDir, sizeof(etTuDir), "%s%s", gSwitchBaseDir, kEtTuSubdir);
        if (dirExists(etTuDir)) {
            snprintf(gameDir, sizeof(gameDir), "%s", etTuDir);
        }
    }

    char configPath[512];
    snprintf(configPath, sizeof(configPath), "%s%s", gSwitchBaseDir, kSwitchConfigFileName);
    gCpuBoostMode = readIniInt(configPath, "CPU_BOOST", 1) == 1 ? 1 : 0;
    gCpuBoostEnabled = gCpuBoostMode == 1;
    gPerfLogEnabled = readIniInt(configPath, "PERF_LOG", 1) == 1;
    gVsyncEnabled = readIniInt(configPath, "VSYNC", 1) == 1;

    if (gCpuBoostEnabled) {
        // Raises the CPU from 1020 MHz to 1785 MHz while the game runs.
        // Only works when launched through a game (title override), not the
        // album applet, so a failure here turns the option off.
        Result rc = appletSetCpuBoostMode(static_cast<ApmCpuBoostMode>(gCpuBoostMode));
        if (R_SUCCEEDED(rc)) {
            atexit(restoreCpuClock);
        } else {
            gCpuBoostEnabled = false;
            gCpuBoostMode = 0;
        }
    }

    chdir(gameDir);
}

const char* switchGetBaseDir()
{
    return gSwitchBaseDir;
}

bool switchWantsVsync()
{
    return gVsyncEnabled;
}

void switchApplyScreenOverrides(int* width, int* height, int* scale, WindowMode* mode)
{
    *width = kDefaultWidth;
    *height = kDefaultHeight;
    *scale = kDefaultScale;
    *mode = WindowMode::Fullscreen;

    char path[512];
    snprintf(path, sizeof(path), "%s%s", gSwitchBaseDir, kSwitchConfigFileName);

    Config config;
    if (!configInit(&config)) {
        return;
    }

    if (configRead(&config, path, false)) {
        configGetInt(&config, "MAIN", "SCR_WIDTH", width);
        configGetInt(&config, "MAIN", "SCR_HEIGHT", height);

        bool scale2x;
        if (configGetBool(&config, "MAIN", "SCALE_2X", &scale2x)) {
            *scale = scale2x ? 2 : 1;
        }
    }

    configFree(&config);
}

// MARK: Keyboard

typedef struct ButtonBinding {
    u64 button;
    SDL_Scancode scancode;
} ButtonBinding;

static const ButtonBinding kButtonBindings[] = {
    // Start combat
    { HidNpadButton_A, SDL_SCANCODE_A },
    // End turn
    { HidNpadButton_B, SDL_SCANCODE_SPACE },
    // Skilldex
    { HidNpadButton_X, SDL_SCANCODE_S },
    // Inventory
    { HidNpadButton_Y, SDL_SCANCODE_I },
    // Options menu
    { HidNpadButton_Plus, SDL_SCANCODE_ESCAPE },
    // Character screen
    { HidNpadButton_Minus, SDL_SCANCODE_C },
    // Sneak
    { HidNpadButton_StickL, SDL_SCANCODE_1 },
    // End combat
    { HidNpadButton_StickR, SDL_SCANCODE_KP_ENTER },
    // Pip-Boy
    { HidNpadButton_Up, SDL_SCANCODE_P },
    // Center on player
    { HidNpadButton_Down, SDL_SCANCODE_HOME },
    // Hold to highlight items. Fallout 2 mods usually use Shift, Fallout et
    // tu uses Ctrl, so hold both.
    { HidNpadButton_Left, SDL_SCANCODE_LSHIFT },
    { HidNpadButton_Left, SDL_SCANCODE_LCTRL },
};

static const SDL_Scancode kArrowScancodes[] = {
    SDL_SCANCODE_UP,
    SDL_SCANCODE_DOWN,
    SDL_SCANCODE_LEFT,
    SDL_SCANCODE_RIGHT,
};

static bool gArrowHeld[4] = { false, false, false, false };

static void pushKeyEvent(SDL_Scancode scancode, bool down)
{
    SDL_Event event;
    SDL_zero(event);
    event.type = down ? SDL_KEYDOWN : SDL_KEYUP;
    event.key.state = down ? SDL_PRESSED : SDL_RELEASED;
    event.key.repeat = 0;
    event.key.keysym.scancode = scancode;
    event.key.keysym.sym = SDL_GetKeyFromScancode(scancode);
    SDL_PushEvent(&event);
}

static void releaseAllKeys()
{
    u64 held = padGetButtons(&gKeyboardPad);
    for (const ButtonBinding& binding : kButtonBindings) {
        if ((held & binding.button) != 0) {
            pushKeyEvent(binding.scancode, false);
        }
    }

    for (int index = 0; index < 4; index++) {
        if (gArrowHeld[index]) {
            pushKeyEvent(kArrowScancodes[index], false);
            gArrowHeld[index] = false;
        }
    }

    gCursorSpeedup = 1.0;
}

// Right stick scrolls the map. The strongest direction wins, the others are
// released. A held direction is re-sent every poll so the map scrolls
// continuously, same as the previous Switch release.
static void handleRightStick()
{
    HidAnalogStickState stick = padGetStickPos(&gKeyboardPad, 1);

    int active = -1;
    if (stick.y > kRightStickDeadZone && abs(stick.y) > abs(stick.x)) {
        active = 0;
    } else if (stick.y < -kRightStickDeadZone && abs(stick.y) > abs(stick.x)) {
        active = 1;
    } else if (stick.x < -kRightStickDeadZone) {
        active = 2;
    } else if (stick.x > kRightStickDeadZone) {
        active = 3;
    }

    for (int index = 0; index < 4; index++) {
        if (index == active) {
            pushKeyEvent(kArrowScancodes[index], true);
            gArrowHeld[index] = true;
        } else if (gArrowHeld[index]) {
            pushKeyEvent(kArrowScancodes[index], false);
            gArrowHeld[index] = false;
        }
    }
}

void switchInputInit()
{
    if (gSwitchInputInitialized) {
        return;
    }

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&gKeyboardPad);
    padInitializeDefault(&gMousePad);

    gSwitchInputInitialized = true;
}

void switchInputPoll(bool textInputActive)
{
    if (!gSwitchInputInitialized) {
        return;
    }

    static bool wasTextInputActive = false;

    if (textInputActive) {
        if (!wasTextInputActive) {
            releaseAllKeys();
            wasTextInputActive = true;
        }
        return;
    }

    if (wasTextInputActive) {
        // Swallow whatever was pressed while the on-screen keyboard was up.
        padUpdate(&gKeyboardPad);
        wasTextInputActive = false;

        if (gCpuBoostEnabled) {
            applyCpuBoost();
        }

        return;
    }

    padUpdate(&gKeyboardPad);

    if (gCpuBoostEnabled) {
        static unsigned long long lastBoostCounter = 0;
        unsigned long long now = SDL_GetPerformanceCounter();
        if (lastBoostCounter == 0 || now - lastBoostCounter >= 2 * SDL_GetPerformanceFrequency()) {
            applyCpuBoost();
            lastBoostCounter = now;
        }
    }

    u64 down = padGetButtonsDown(&gKeyboardPad);
    u64 up = padGetButtonsUp(&gKeyboardPad);
    u64 held = padGetButtons(&gKeyboardPad);

    for (const ButtonBinding& binding : kButtonBindings) {
        if ((down & binding.button) != 0) {
            pushKeyEvent(binding.scancode, true);
        }

        if ((up & binding.button) != 0) {
            pushKeyEvent(binding.scancode, false);
        }
    }

    // Hold R to move the cursor faster.
    gCursorSpeedup = (held & HidNpadButton_R) != 0 ? 2.0 : 1.0;

    handleRightStick();
}

// MARK: Mouse

void switchMouseApply(MouseData* mouseData)
{
    if (!gSwitchInputInitialized) {
        return;
    }

    padUpdate(&gMousePad);

    // Fractional movement carried over between polls, so slow stick
    // movement and low sensitivity still move the cursor.
    static double remainderX = 0.0;
    static double remainderY = 0.0;
    static unsigned long long lastPollCounter = 0;

    unsigned long long now = SDL_GetPerformanceCounter();
    double elapsedMs = lastPollCounter != 0
        ? (now - lastPollCounter) * 1000.0 / static_cast<double>(SDL_GetPerformanceFrequency())
        : 1000.0 / 60.0;
    lastPollCounter = now;

    HidAnalogStickState stick = padGetStickPos(&gMousePad, 0);
    if (abs(stick.x) > kLeftStickDeadZone || abs(stick.y) > kLeftStickDeadZone) {
        // Roughly the same curve as the previous Switch release. The engine
        // applies mouse sensitivity again afterwards, same as before.
        double sensitivity = mouseGetSensitivity();
        if (sensitivity < 1.0) {
            sensitivity = 1.0;
        }

        // Scale by time since the last poll, so the cursor moves at the same
        // speed whether a screen runs at 60 fps or, like the save screen,
        // at 24 fps. Tuned to match the old per-poll speed at 60 fps.
        double frames = elapsedMs / (1000.0 / 60.0);
        if (frames > 4.0) {
            frames = 4.0;
        }

        double speed = gCursorSpeedup * sensitivity * 1.5 * frames;
        double moveX = (stick.x / 10000.0) * speed + remainderX;
        double moveY = -(stick.y / 10000.0) * speed + remainderY;

        int stepX = static_cast<int>(moveX);
        int stepY = static_cast<int>(moveY);
        remainderX = moveX - stepX;
        remainderY = moveY - stepY;

        mouseData->x += stepX;
        mouseData->y += stepY;
    } else {
        remainderX = 0.0;
        remainderY = 0.0;
    }

    u64 held = padGetButtons(&gMousePad);
    mouseData->buttons[0] |= (held & HidNpadButton_ZL) != 0;
    mouseData->buttons[1] |= (held & HidNpadButton_ZR) != 0;
}

// MARK: Performance log

// Defined in animation.cc.
int switchPerfBusyAnimationSequences();
int switchPerfBusyAnimationMoves();

static const char* const kPerfZoneNames[] = {
    "input",
    "window_refresh",
    "map_render",
    "scripts",
    "global_scripts",
    "programs",
    "sound",
};

static const int kPerfZoneCount = static_cast<int>(SwitchPerfZoneId::Count);
static const int kPerfTopFileCount = 8;

typedef struct PerfFileStats {
    int count;
    int missing;
    double totalMs;
} PerfFileStats;

static unsigned long long gPerfWindowStart = 0;
static unsigned long long gPerfLastPresentEnd = 0;
static unsigned long long gPerfProgramStart = 0;
static int gPerfFrames = 0;
static double gPerfFrameTotalMs = 0.0;
static double gPerfFrameMaxMs = 0.0;
static double gPerfPresentTotalMs = 0.0;
static double gPerfPresentMaxMs = 0.0;
static double gPerfZoneTotalMs[kPerfZoneCount] = {};
static int gPerfZoneDepth[kPerfZoneCount] = {};
static int gPerfFileOpens = 0;
static double gPerfFileOpenTotalMs = 0.0;
static double gPerfFileOpenMaxMs = 0.0;
static int gPerfDirectoryScans = 0;
static double gPerfDirectoryScanTotalMs = 0.0;
static double gPerfDirectoryScanMaxMs = 0.0;
static char gPerfDirectoryScanMaxPath[256] = "";
static int gPerfDirectoryCacheClears = 0;
static int gPerfFileOpenErrors = 0;
static int gPerfFileOpenLastError = 0;
static char gPerfFileOpenLastErrorPath[256] = "";
static FILE* gPerfLogStream = nullptr;
static int gPerfFocusLost = 0;
static int gPerfFocusGained = 0;
static int gPerfSizeChanged = 0;
static int gPerfOtherWindowEvents = 0;
static int gPerfRendererRebuilds = 0;

// Picture changes: frames where something on screen was redrawn.
static bool gPerfScreenUpdatedThisFrame = false;
static int gPerfChangedFrames = 0;
static unsigned long long gPerfLastChangedFrameCounter = 0;
static double gPerfChangeGapMaxMs = 0.0;
static int gPerfChangeGapsOver50Ms = 0;
static int gPerfChangeGapsOver100Ms = 0;

// How many times each timed part of the game ran, per window.
static int gPerfZoneCalls[static_cast<int>(SwitchPerfZoneId::Count)] = {};
static std::unordered_map<std::string, PerfFileStats> gPerfFileStats;

static double perfMsSince(unsigned long long startCounter)
{
    unsigned long long now = SDL_GetPerformanceCounter();
    return (now - startCounter) * 1000.0 / static_cast<double>(SDL_GetPerformanceFrequency());
}

SwitchPerfZone::SwitchPerfZone(SwitchPerfZoneId id)
    : _id(static_cast<int>(id))
    , _start(0)
    , _outermost(false)
{
    if (!gPerfLogEnabled) {
        return;
    }

    _outermost = gPerfZoneDepth[_id] == 0;
    gPerfZoneDepth[_id]++;

    if (_outermost) {
        _start = SDL_GetPerformanceCounter();
        gPerfZoneCalls[_id]++;
    }
}

SwitchPerfZone::~SwitchPerfZone()
{
    if (!gPerfLogEnabled) {
        return;
    }

    gPerfZoneDepth[_id]--;

    if (_outermost) {
        gPerfZoneTotalMs[_id] += perfMsSince(_start);
    }
}

void switchPerfRecordFileOpen(const char* path, unsigned long long startCounter, bool found)
{
    if (!gPerfLogEnabled) {
        return;
    }

    double ms = perfMsSince(startCounter);

    gPerfFileOpens++;
    gPerfFileOpenTotalMs += ms;
    if (ms > gPerfFileOpenMaxMs) {
        gPerfFileOpenMaxMs = ms;
    }

    PerfFileStats& stats = gPerfFileStats[path];
    stats.count++;
    stats.totalMs += ms;
    if (!found) {
        stats.missing++;
    }
}

void switchPerfRecordDirectoryScan(const char* path, unsigned long long startCounter)
{
    if (!gPerfLogEnabled) {
        return;
    }

    double ms = perfMsSince(startCounter);

    gPerfDirectoryScans++;
    gPerfDirectoryScanTotalMs += ms;
    if (ms > gPerfDirectoryScanMaxMs) {
        gPerfDirectoryScanMaxMs = ms;
        snprintf(gPerfDirectoryScanMaxPath, sizeof(gPerfDirectoryScanMaxPath), "%s", path);
    }
}

void switchPerfRecordFileOpenError(const char* path, int error)
{
    if (!gPerfLogEnabled) {
        return;
    }

    gPerfFileOpenErrors++;
    gPerfFileOpenLastError = error;
    snprintf(gPerfFileOpenLastErrorPath, sizeof(gPerfFileOpenLastErrorPath), "%s", path);
}

void switchPerfCountWindowEvent(int windowEvent)
{
    if (!gPerfLogEnabled) {
        return;
    }

    switch (windowEvent) {
    case SDL_WINDOWEVENT_FOCUS_LOST:
        gPerfFocusLost++;
        break;
    case SDL_WINDOWEVENT_FOCUS_GAINED:
        gPerfFocusGained++;
        break;
    case SDL_WINDOWEVENT_SIZE_CHANGED:
    case SDL_WINDOWEVENT_RESIZED:
        gPerfSizeChanged++;
        break;
    default:
        gPerfOtherWindowEvents++;
        break;
    }
}

void switchPerfMarkScreenUpdate()
{
    gPerfScreenUpdatedThisFrame = true;
}

void switchPerfCountRendererRebuild()
{
    if (!gPerfLogEnabled) {
        return;
    }

    gPerfRendererRebuilds++;
}

void switchPerfCountDirectoryCacheClear()
{
    if (!gPerfLogEnabled) {
        return;
    }

    gPerfDirectoryCacheClears++;
}

static void perfResetWindow(unsigned long long now)
{
    gPerfWindowStart = now;
    gPerfFrames = 0;
    gPerfFrameTotalMs = 0.0;
    gPerfFrameMaxMs = 0.0;
    gPerfPresentTotalMs = 0.0;
    gPerfPresentMaxMs = 0.0;

    for (int index = 0; index < kPerfZoneCount; index++) {
        gPerfZoneTotalMs[index] = 0.0;
        gPerfZoneCalls[index] = 0;
    }

    gPerfChangedFrames = 0;
    gPerfChangeGapMaxMs = 0.0;
    gPerfChangeGapsOver50Ms = 0;
    gPerfChangeGapsOver100Ms = 0;

    gPerfFileOpens = 0;
    gPerfFileOpenTotalMs = 0.0;
    gPerfFileOpenMaxMs = 0.0;
    gPerfDirectoryScans = 0;
    gPerfDirectoryScanTotalMs = 0.0;
    gPerfDirectoryScanMaxMs = 0.0;
    gPerfDirectoryScanMaxPath[0] = '\0';
    gPerfDirectoryCacheClears = 0;
    gPerfFileOpenErrors = 0;
    gPerfFileOpenLastError = 0;
    gPerfFileOpenLastErrorPath[0] = '\0';
    gPerfFocusLost = 0;
    gPerfFocusGained = 0;
    gPerfSizeChanged = 0;
    gPerfOtherWindowEvents = 0;
    gPerfRendererRebuilds = 0;
    gPerfFileStats.clear();
}

static void perfWriteWindow(FILE* stream, double elapsedSeconds, double windowSeconds)
{
    int frames = gPerfFrames > 0 ? gPerfFrames : 1;

    fprintf(stream,
        "t=%.0fs cpu_boost=%d vsync=%d fps=%.1f frame_avg=%.1fms frame_max=%.1fms present_avg=%.1fms present_max=%.1fms\n",
        elapsedSeconds,
        gCpuBoostEnabled ? gCpuBoostMode : 0,
        gVsyncEnabled ? 1 : 0,
        gPerfFrames / windowSeconds,
        gPerfFrameTotalMs / frames,
        gPerfFrameMaxMs,
        gPerfPresentTotalMs / frames,
        gPerfPresentMaxMs);

    fprintf(stream, "  per frame:");
    for (int index = 0; index < kPerfZoneCount; index++) {
        fprintf(stream, " %s=%.1fms", kPerfZoneNames[index], gPerfZoneTotalMs[index] / frames);
    }
    fprintf(stream, "\n");

    fprintf(stream, "  calls per second:");
    for (int index = 0; index < kPerfZoneCount; index++) {
        fprintf(stream, " %s=%.1f", kPerfZoneNames[index], gPerfZoneCalls[index] / windowSeconds);
    }
    fprintf(stream, "\n");

    fprintf(stream,
        "  picture: changed_fps=%.1f longest_gap=%.1fms gaps_over_50ms=%d gaps_over_100ms=%d\n",
        gPerfChangedFrames / windowSeconds,
        gPerfChangeGapMaxMs,
        gPerfChangeGapsOver50Ms,
        gPerfChangeGapsOver100Ms);

    fprintf(stream,
        "  animation slots: sequences_busy=%d/64 moves_busy=%d/64\n",
        switchPerfBusyAnimationSequences(),
        switchPerfBusyAnimationMoves());

    fprintf(stream,
        "  files: opens=%d open_total=%.0fms open_max=%.1fms dir_scans=%d dir_scan_total=%.0fms dir_scan_max=%.1fms (%s) cache_clears=%d\n",
        gPerfFileOpens,
        gPerfFileOpenTotalMs,
        gPerfFileOpenMaxMs,
        gPerfDirectoryScans,
        gPerfDirectoryScanTotalMs,
        gPerfDirectoryScanMaxMs,
        gPerfDirectoryScanMaxPath,
        gPerfDirectoryCacheClears);

    // Current clock setting from the system, for example 0x9222000a means
    // boosted CPU with lowered graphics speed (handheld).
    ApmPerformanceMode performanceMode = ApmPerformanceMode_Normal;
    u32 performanceConfiguration = 0;
    Result modeResult = apmGetPerformanceMode(&performanceMode);
    Result configResult = R_SUCCEEDED(modeResult)
        ? apmGetPerformanceConfiguration(performanceMode, &performanceConfiguration)
        : modeResult;

    fprintf(stream,
        "  clocks: mode=%d config=0x%x (result=0x%x) cpu_boost_failures=%u last_boost_error=0x%x open_errors=%d last_open_error=%d (%s)\n",
        static_cast<int>(performanceMode),
        performanceConfiguration,
        configResult,
        gCpuBoostFailures,
        gCpuBoostLastError,
        gPerfFileOpenErrors,
        gPerfFileOpenLastError,
        gPerfFileOpenLastErrorPath);

    int windowWidth = 0;
    int windowHeight = 0;
    int outputWidth = 0;
    int outputHeight = 0;
    Uint32 windowFlags = 0;
    SDL_Window* window = SDL_GL_GetCurrentWindow();
    if (window == nullptr) {
        window = SDL_GetWindowFromID(1);
    }
    if (window != nullptr) {
        SDL_GetWindowSize(window, &windowWidth, &windowHeight);
        windowFlags = SDL_GetWindowFlags(window);
        SDL_Renderer* renderer = SDL_GetRenderer(window);
        if (renderer != nullptr) {
            SDL_GetRendererOutputSize(renderer, &outputWidth, &outputHeight);
        }
    }

    fprintf(stream,
        "  screen: window=%dx%d output=%dx%d flags=0x%x focus_lost=%d focus_gained=%d size_changed=%d other_window_events=%d renderer_rebuilds=%d\n",
        windowWidth,
        windowHeight,
        outputWidth,
        outputHeight,
        windowFlags,
        gPerfFocusLost,
        gPerfFocusGained,
        gPerfSizeChanged,
        gPerfOtherWindowEvents,
        gPerfRendererRebuilds);

    if (gPerfFileStats.empty()) {
        return;
    }

    std::vector<std::pair<std::string, PerfFileStats>> files(gPerfFileStats.begin(), gPerfFileStats.end());
    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) {
        return a.second.totalMs > b.second.totalMs;
    });

    int shown = 0;
    for (const auto& file : files) {
        if (shown >= kPerfTopFileCount) {
            break;
        }

        fprintf(stream,
            "  slowest file: %s count=%d missing=%d total=%.1fms\n",
            file.first.c_str(),
            file.second.count,
            file.second.missing,
            file.second.totalMs);
        shown++;
    }
}

void switchPerfRecordPresent(unsigned long long presentStartCounter)
{
    if (!gPerfLogEnabled) {
        return;
    }

    unsigned long long now = SDL_GetPerformanceCounter();
    double frequency = static_cast<double>(SDL_GetPerformanceFrequency());

    if (gPerfProgramStart == 0) {
        gPerfProgramStart = now;
        gPerfLastPresentEnd = now;
        perfResetWindow(now);
        return;
    }

    double presentMs = (now - presentStartCounter) * 1000.0 / frequency;
    double frameMs = (now - gPerfLastPresentEnd) * 1000.0 / frequency;
    gPerfLastPresentEnd = now;

    if (gPerfScreenUpdatedThisFrame) {
        if (gPerfLastChangedFrameCounter != 0) {
            double gapMs = (now - gPerfLastChangedFrameCounter) * 1000.0 / frequency;
            if (gapMs > gPerfChangeGapMaxMs) {
                gPerfChangeGapMaxMs = gapMs;
            }
            if (gapMs > 50.0) {
                gPerfChangeGapsOver50Ms++;
            }
            if (gapMs > 100.0) {
                gPerfChangeGapsOver100Ms++;
            }
        }

        gPerfLastChangedFrameCounter = now;
        gPerfChangedFrames++;
        gPerfScreenUpdatedThisFrame = false;
    }

    gPerfFrames++;
    gPerfFrameTotalMs += frameMs;
    gPerfPresentTotalMs += presentMs;
    if (frameMs > gPerfFrameMaxMs) {
        gPerfFrameMaxMs = frameMs;
    }
    if (presentMs > gPerfPresentMaxMs) {
        gPerfPresentMaxMs = presentMs;
    }

    double windowSeconds = (now - gPerfWindowStart) / frequency;
    if (windowSeconds < 5.0) {
        return;
    }

    // Kept open for the whole session, so logging still works even if the
    // game runs out of file handles. Plain fopen on purpose: the game's own
    // file functions are what's being measured.
    if (gPerfLogStream == nullptr) {
        char path[512];
        snprintf(path, sizeof(path), "%sperf.log", gSwitchBaseDir);
        gPerfLogStream = fopen(path, "at");
    }

    if (gPerfLogStream != nullptr) {
        perfWriteWindow(gPerfLogStream, (now - gPerfProgramStart) / frequency, windowSeconds);
        fflush(gPerfLogStream);
    }

    perfResetWindow(SDL_GetPerformanceCounter());
}

} // namespace fallout

#endif /* __SWITCH__ */
