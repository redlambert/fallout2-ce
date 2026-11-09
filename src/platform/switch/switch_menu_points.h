#ifndef FALLOUT_PLATFORM_SWITCH_SWITCH_MENU_POINTS_H_
#define FALLOUT_PLATFORM_SWITCH_SWITCH_MENU_POINTS_H_

#include "window_manager.h"

namespace fallout {

// Some screens use one big button for a whole list or control and work out
// what was clicked from the cursor position (save slots, character screen
// lists, preference dials and sliders). For D-pad navigation those screens
// report the spots inside that button worth jumping to, in window
// coordinates, using the same math they use to read a click.
struct SwitchMenuPoint {
    int x;
    int y;
};

// Each returns -1 when the button isn't one of its own, otherwise the
// number of points written.
int loadSaveGetMenuPoints(int window, const Button* button, SwitchMenuPoint* points, int capacity);
int characterEditorGetMenuPoints(int window, const Button* button, SwitchMenuPoint* points, int capacity);
int preferencesGetMenuPoints(int window, const Button* button, SwitchMenuPoint* points, int capacity);

} // namespace fallout

#endif /* FALLOUT_PLATFORM_SWITCH_SWITCH_MENU_POINTS_H_ */
