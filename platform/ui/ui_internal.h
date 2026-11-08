/* Between the menu (ui_menu.cpp) and its GL backend (ui_gl.cpp). C++ only. */
#ifndef GUARD_PORT_UI_INTERNAL_H
#define GUARD_PORT_UI_INTERNAL_H

struct ImDrawData;

int UiGlInit(int es);
void UiGlRebuildFont(void);
void UiGlRender(ImDrawData *dd, int fbW, int fbH);
void UiLogError(const char *msg);

#endif
