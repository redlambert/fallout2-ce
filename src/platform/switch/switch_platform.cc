#ifdef __SWITCH__

#include "platform/switch/switch_platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>

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

    chdir(gameDir);
}

const char* switchGetBaseDir()
{
    return gSwitchBaseDir;
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
        return;
    }

    padUpdate(&gKeyboardPad);

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

    HidAnalogStickState stick = padGetStickPos(&gMousePad, 0);
    if (abs(stick.x) > kLeftStickDeadZone || abs(stick.y) > kLeftStickDeadZone) {
        // Roughly the same curve as the previous Switch release. The engine
        // applies mouse sensitivity again afterwards, same as before.
        double sensitivity = mouseGetSensitivity();
        if (sensitivity < 1.0) {
            sensitivity = 1.0;
        }

        double speed = gCursorSpeedup * sensitivity * 1.5;
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

} // namespace fallout

#endif /* __SWITCH__ */
