#ifndef UGUI_CONFIG_H
#define UGUI_CONFIG_H

/* Minimal uGUI config for Quartz ESP32 */

typedef unsigned short UG_COLOR;
typedef unsigned char  UG_U8;
typedef signed char    UG_S8;
typedef unsigned short UG_U16;
typedef signed short   UG_S16;
typedef unsigned int   UG_U32;
typedef signed int     UG_S32;

/* Use RGB565 color format (16-bit, matches ILI9341) */
#define USE_COLOR_RGB565

/* Disable ALL built-in fonts - we use our own DejaVu fonts */
/* (don't define USE_FONT_* at all) */

/* uGUI needs this macro for font data */
#define __UG_FONT_DATA const

#endif /* UGUI_CONFIG_H */
