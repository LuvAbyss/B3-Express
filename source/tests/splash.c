/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The boot splash's drawings: every one built in draws at any size, filling its square as Wimsy does, and Wimsy
   drawn here matches dist/icons' PNG, which rsvg-convert drew from the same SVG */
#include "gui/splash.h"
#include <SDL3/SDL.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the opaque share of a drawing, in thousandths, and the box its visible pixels take */
typedef struct {
    int opaque, left, top, right, bottom;
} Coverage;

static Coverage coverage(const Uint32* pixels, int side) {
    Coverage c = {0, side, side, -1, -1};
    int x, y, opaque = 0;
    for (y = 0; y < side; ++y)
        for (x = 0; x < side; ++x) {
            Uint32 a = pixels[y * side + x] >> 24;
            opaque += a == 255;
            if (!a) continue;
            if (x < c.left) c.left = x;
            if (y < c.top) c.top = y;
            if (x > c.right) c.right = x;
            if (y > c.bottom) c.bottom = y;
        }
    c.opaque = (int)((long long)opaque * 1000 / ((long long)side * side));
    return c;
}

int main(void) {
    static const int sides[] = {1, 64, 240, 333, 1024};
    int i, k, x, y;
    SnxSplashDrawing* drawing;
    Uint32* pixels = malloc(1024 * 1024 * 4);
    assert(pixels);
    /* Wimsy first, then dist/splash's, each one once, as the SVG text without its metadata and comments */
    assert(snx_splash_svg_count >= 2 && !strcmp(snx_splash_svgs[0].name, "wimsy"));
    for (i = 0; i < snx_splash_svg_count; ++i) {
        const SnxSplashSvg* svg = &snx_splash_svgs[i];
        char* text = malloc(svg->size + 1);
        assert(text);
        memcpy(text, svg->text, svg->size);
        text[svg->size] = 0;
        assert(!strncmp(text, "<svg ", 5) && !strcmp(text + svg->size - 6, "</svg>"));
        assert(!strstr(text, "<metadata") && !strstr(text, "<!--") && !strstr(text, "\n"));
        for (k = 0; k < i; ++k) assert(strcmp(snx_splash_svgs[k].name, svg->name));
        free(text);
        /* read once, drawn at every size from that */
        drawing = snx_splash_open(svg);
        assert(drawing);
        for (k = 0; k < (int)(sizeof sides / sizeof *sides); ++k) {
            int side = sides[k];
            Coverage c;
            memset(pixels, 0xAB, (size_t)side * side * 4); /* what was there goes */
            assert(!snx_splash_draw(drawing, side, pixels, side * 4));
            c = coverage(pixels, side);
            if (side < 64) continue;
            /* the corners clear, about a third of the square opaque (less when small, its edges blended), and the leaves
               reaching its sides */
            assert(!pixels[0] && !pixels[side - 1] && !pixels[(side - 1) * side] && !pixels[side * side - 1]);
            if (c.opaque < 220 || c.opaque > 450 || c.left > side / 50 || c.right < side - 1 - side / 50 ||
                c.top > side / 40 || c.bottom < side - 1 - side / 40) {
                fprintf(stderr, "%s at %d: %d/1000 opaque, from %d,%d to %d,%d\n", svg->name, side, c.opaque, c.left,
                        c.top, c.right, c.bottom);
                assert(0);
            }
        }
        snx_splash_close(drawing);
    }
    /* Wimsy as dist/icons' 256 PNG has it: the colours (premultiplied) under a step apart on average, a few edge pixels
       more */
    {
        SDL_Surface* png = SDL_LoadPNG(SNX_SPLASH_ICON);
        long long off = 0;
        int far = 0;
        assert(png && png->w == 256 && png->h == 256);
        drawing = snx_splash_open(&snx_splash_svgs[0]);
        assert(drawing && !snx_splash_draw(drawing, 256, pixels, 256 * 4));
        snx_splash_close(drawing);
        for (y = 0; y < 256; ++y)
            for (x = 0; x < 256; ++x) {
                Uint8 r, g, b, a;
                Uint32 p = pixels[y * 256 + x];
                int ours[4], theirs[4], c, worst = 0;
                assert(SDL_ReadSurfacePixel(png, x, y, &r, &g, &b, &a));
                ours[0] = (int)(p >> 16 & 255), ours[1] = (int)(p >> 8 & 255), ours[2] = (int)(p & 255);
                ours[3] = (int)(p >> 24);
                theirs[0] = r * a / 255, theirs[1] = g * a / 255, theirs[2] = b * a / 255, theirs[3] = a;
                for (c = 0; c < 4; ++c) {
                    int d = abs(ours[c] - theirs[c]);
                    off += d;
                    if (d > worst) worst = d;
                }
                far += worst > 24;
            }
        printf("Wimsy against rsvg: %.3f mean, %d of 65536 pixels over 24\n", (double)off / (256 * 256 * 4), far);
        assert(off < 256 * 256 * 4 / 2 && far < 300);
        SDL_DestroySurface(png);
    }
    /* what it cannot draw it says, drawing nothing */
    {
        SnxSplashSvg broken = {"broken", "not a drawing", 13};
        assert(!snx_splash_open(&broken) && !snx_splash_open(NULL));
        assert(snx_splash_draw(NULL, 64, pixels, 64 * 4));
        drawing = snx_splash_open(&snx_splash_svgs[0]);
        assert(drawing && snx_splash_draw(drawing, 0, pixels, 0));
        assert(snx_splash_draw(drawing, 64, pixels, 63 * 4) && snx_splash_draw(drawing, 64, NULL, 64 * 4));
        snx_splash_close(drawing);
        snx_splash_close(NULL);
    }
    free(pixels);
    return 0;
}
