/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* pad_map.h's controls on SDL's gamepads, for the runtime (gamepad.c) and the launcher (cmake/pad_sdl.cmake) */
#ifndef SNX_PAD_SDL_H
#define SNX_PAD_SDL_H
#include <SDL3/SDL.h>
#include "common/utilities/pad_map.h"

#define SNX_PAD_TRIGGER 8000 /* how far a trigger goes before it is held */
/* the control an SDL button is, -1 for none (Home, the extra ones) */
int snx_pad_of_button(int button);
/* whether g holds control now */
int snx_pad_held(SDL_Gamepad* g, int control);
/* g's face labels (SNX_PAD_LABEL_ of south, east, west, north), and whether its sticks sit side by side */
void snx_pad_labels(SDL_Gamepad* g, int labels[4]);
int snx_pad_symmetric(SDL_Gamepad* g);
#endif
