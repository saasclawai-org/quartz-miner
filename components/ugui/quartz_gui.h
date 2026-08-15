/**
 * quartz_gui.h — uGUI integration for Quartz M5Stack display
 */
#ifndef QUARTZ_GUI_H
#define QUARTZ_GUI_H

#include "ugui.h"

#ifdef ESP_PLATFORM
void quartz_gui_init(void);
void quartz_gui_clear(UG_COLOR c);
UG_GUI* quartz_gui_get(void);

/* Fonts */
extern const UG_FONT FONT_DEJAVU14;
extern const UG_FONT FONT_DEJAVU18;
extern const UG_FONT FONT_DEJAVU24;
#endif

#endif /* QUARTZ_GUI_H */