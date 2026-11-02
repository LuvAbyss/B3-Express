/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/utilities/pad_sdl.h"

/* each control's button; the triggers are axes */
static const SDL_GamepadButton BUTTONS[SNX_PAD_CONTROLS] = {
    SDL_GAMEPAD_BUTTON_SOUTH,       SDL_GAMEPAD_BUTTON_EAST,          SDL_GAMEPAD_BUTTON_WEST,
    SDL_GAMEPAD_BUTTON_NORTH,       SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,
    SDL_GAMEPAD_BUTTON_INVALID,     SDL_GAMEPAD_BUTTON_INVALID,       SDL_GAMEPAD_BUTTON_LEFT_STICK,
    SDL_GAMEPAD_BUTTON_RIGHT_STICK, SDL_GAMEPAD_BUTTON_BACK,          SDL_GAMEPAD_BUTTON_START,
    SDL_GAMEPAD_BUTTON_DPAD_UP,     SDL_GAMEPAD_BUTTON_DPAD_DOWN,     SDL_GAMEPAD_BUTTON_DPAD_LEFT,
    SDL_GAMEPAD_BUTTON_DPAD_RIGHT};
/* pad_map.h's labels are SDL's, in its order */
typedef char snx_pad_labels_sdl[SDL_GAMEPAD_BUTTON_LABEL_TRIANGLE == SNX_PAD_LABEL_TRIANGLE &&
                                        SDL_GAMEPAD_BUTTON_LABEL_A == SNX_PAD_LABEL_A
                                    ? 1
                                    : -1];

int snx_pad_of_button(int button) {
    int c;
    for (c = 0; c < SNX_PAD_CONTROLS; ++c)
        if (BUTTONS[c] != SDL_GAMEPAD_BUTTON_INVALID && (int)BUTTONS[c] == button) return c;
    return -1;
}
int snx_pad_held(SDL_Gamepad* g, int control) {
    if (control == SNX_PAD_LT || control == SNX_PAD_RT)
        return SDL_GetGamepadAxis(g, control == SNX_PAD_LT ? SDL_GAMEPAD_AXIS_LEFT_TRIGGER
                                                           : SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > SNX_PAD_TRIGGER;
    return control >= 0 && control < SNX_PAD_CONTROLS && SDL_GetGamepadButton(g, BUTTONS[control]);
}
void snx_pad_labels(SDL_Gamepad* g, int labels[4]) {
    int c;
    for (c = SNX_PAD_SOUTH; c <= SNX_PAD_NORTH; ++c) labels[c] = (int)SDL_GetGamepadButtonLabel(g, BUTTONS[c]);
}
int snx_pad_symmetric(SDL_Gamepad* g) {
    switch (SDL_GetGamepadType(g)) {
        case SDL_GAMEPAD_TYPE_PS3:
        case SDL_GAMEPAD_TYPE_PS4:
        case SDL_GAMEPAD_TYPE_PS5: return 1;
        default: return 0;
    }
}
