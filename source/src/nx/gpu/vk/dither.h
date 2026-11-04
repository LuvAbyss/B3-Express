/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The game's screen-door fade (dither=0..3 in the INI): see dither.c */
#pragma once
#include <stddef.h>
#include <stdint.h>

enum { DITHER_GAME = 0, DITHER_OFF = 1, DITHER_EVEN = 2, DITHER_BLEND = 3 };

/* A pixel stage's SPIR-V, as the translator made it. Finds the game's fade (a 64 x 64 pattern fetched at the pixel's
   position, and a discard when the object's alpha is below it) and changes it for mode. *spv may be replaced (the old
   one freed) and *words with it. fade_test >= 0 (SNX_DITHER_FADE, test only) puts that alpha in every fade found, so
   the pattern shows on every object that has it. Returns the fade compares found (0: not such a stage); with
   DITHER_BLEND, *blend is set when the stage now writes its alpha as the second blend source (location 0, index 1)
   and draws with it should blend by it. */
int dither_patch(uint32_t** spv, size_t* words, int mode, float fade_test, int* blend);
