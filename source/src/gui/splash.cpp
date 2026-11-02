/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "gui/splash.h"
#include <lunasvg.h>
#include <cstdint>
#include <memory>
#include <new>

struct SnxSplashDrawing {
    std::unique_ptr<lunasvg::Document> document;
};

SnxSplashDrawing* snx_splash_open(const SnxSplashSvg* svg) {
    if (!svg) return nullptr;
    std::unique_ptr<lunasvg::Document> document = lunasvg::Document::loadFromData(svg->text, svg->size);
    if (!document || document->width() <= 0 || document->height() <= 0) return nullptr;
    return new (std::nothrow) SnxSplashDrawing{std::move(document)};
}

int snx_splash_draw(const SnxSplashDrawing* drawing, int side, void* pixels, int stride) {
    if (!drawing || side <= 0 || !pixels || stride < side * 4) return 1;
    const lunasvg::Document& document = *drawing->document;
    lunasvg::Bitmap bitmap(static_cast<uint8_t*>(pixels), side, side, stride);
    bitmap.clear(0);
    /* as renderToBitmap scales it, into our own pixels */
    document.render(bitmap, lunasvg::Matrix(side / document.width(), 0, 0, side / document.height(), 0, 0));
    return 0;
}

void snx_splash_close(SnxSplashDrawing* drawing) { delete drawing; }
