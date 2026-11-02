/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The boot splash's drawings: Wimsy and dist/splash's, compiled in as SVG text (cmake/splash.cmake), and drawn at the
   splash's size by lunasvg (splash.cpp) */
#ifndef SNX_GUI_SPLASH_H
#define SNX_GUI_SPLASH_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char* name; /* its file's, without .svg */
    const char* text;
    size_t size;
} SnxSplashSvg;
extern const SnxSplashSvg snx_splash_svgs[]; /* Wimsy's own first */
extern const int snx_splash_svg_count;

/* svg's drawing, read once to be drawn at any size; NULL when its text is not one */
typedef struct SnxSplashDrawing SnxSplashDrawing;
SnxSplashDrawing* snx_splash_open(const SnxSplashSvg* svg);
/* the drawing filling a side x side square of pixels, ARGB8888 with the alpha premultiplied, stride bytes a row, the
   rest of it transparent; 0 when drawn */
int snx_splash_draw(const SnxSplashDrawing* drawing, int side, void* pixels, int stride);
void snx_splash_close(SnxSplashDrawing* drawing);

#ifdef __cplusplus
}
#endif
#endif
