/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Bayonetta 3 v1.2.0's play camera turned by the mouse itself, not by a right stick made from it.

   The right stick reaches the play camera through copies: the input module's pad object, the engine's pad arrays
   (main+0x95f208), the play copy (main+0x95f35c), the player's input component (player+0x1780, main+0x559c2c), then
   the camera's update (main+0x63c148..0x63c890). That turns the stick into a step for the frame, after a dead zone
   of 200 of 1000, a response curve, the menu's camera speed and the frame's time, and adds the step to the camera's
   two angles: main+0x63c830 (fadd s0, s0, s1) to its pitch at +0x6a0, which it clamps after, and main+0x63c840
   (fadd s1, s1, s2) to its yaw at +0x6a4. The curve is why a mouse made into a stick feels like a stick.

   Hooked before those two adds, each step gets the mouse's motion since the last update in radians: a camera that
   turns exactly as far as the mouse moved, whatever the frame rate. A controller's stick still adds its own step.
   The game's own gates come first: x21 (pitch) and x20 (yaw) are 0 where the camera doesn't take the stick, and
   there the mouse turns nothing either. While this camera runs, the backend stops making the mouse into the right
   stick (backend_mouse_look); other cameras, with no such hook, still get the stick as before.

   Which add is which, and their signs, were seen in unattended runs with SNX_MOUSE_LOOK_TEST (docs/STATUS.md, "The
   mouse turns the play camera itself").

   The hook points are in launcher/code-insnhooks.txt: with a game module compiled without them the hooks never run,
   and the mouse stays a stick */
#include "nx/hle/mouse_look.h"
#include "common/cpu/aarch64/cpu.h"
#include "nx/memory/mem.h"
#include "nx/runtime/loader.h"
#include "nx/runtime/rtapi.h"
#include "nx/runtime/platform/backend.h"
#include "nx/runtime/platform/log.h"

#include <stdlib.h>
#include <string.h>

#define MAIN_FROM_LOW 0x5000   /* main loads right after rtld */
#define ADD_PITCH     0x63c830 /* fadd s0, s0, s1: the pitch, and its step */
#define ADD_YAW       0x63c840 /* fadd s1, s1, s2: the yaw, and its step */
#define WORD_PITCH    0x1e212800u
#define WORD_YAW      0x1e222821u
#define RADIANS       0.0015f /* a count of the mouse at mouse_sensitivity 100: 360 degrees in about 4200 counts */
#define YAW_SIGN      -1.0f   /* the yaw's step for the mouse to the right: a smaller yaw turns right */

static u64 g_main;
static f32 g_yaw;     /* the yaw of the motion taken at ADD_PITCH, for ADD_YAW right after it in the same update */
static f32 g_test[2]; /* SNX_MOUSE_LOOK_TEST=x,y: counts a camera update, for unattended runs */

static void add_pitch(CPU* RESTRICT c, u64 pc) {
    f32 dx = 0, dy = 0;
    (void)pc;
    g_yaw = 0;
    if (!backend_mouse_look(&dx, &dy)) {
        dx = g_test[0];
        dy = g_test[1];
    }
    if (c->x[21]) c->v[1].f[0] -= dy * RADIANS; /* the mouse down looks down */
    if (c->x[20]) g_yaw = YAW_SIGN * dx * RADIANS;
}
static void add_yaw(CPU* RESTRICT c, u64 pc) {
    (void)pc;
    c->v[2].f[0] += g_yaw;
    g_yaw = 0;
}

void mouse_look_init(void) {
    u32 wp = 0, wy = 0;
    const char* test = getenv("SNX_MOUSE_LOOK_TEST");
    g_main = g_code_lo + MAIN_FROM_LOW;
    if (g_main + ADD_YAW >= g_code_hi) return;
    memcpy(&wp, GP(g_main + ADD_PITCH), sizeof wp);
    memcpy(&wy, GP(g_main + ADD_YAW), sizeof wy);
    if (wp != WORD_PITCH || wy != WORD_YAW) {
        LOG("mouse_look: this isn't the game it knows, the mouse turns the camera as a stick\n");
        return;
    }
    if (!rt_hook_set(g_main + ADD_PITCH, add_pitch) || !rt_hook_set(g_main + ADD_YAW, add_yaw)) {
        LOG("mouse_look: no room for its hooks, the mouse turns the camera as a stick\n");
        return;
    }
    if (test) {
        g_test[0] = (f32)atof(test);
        test = strchr(test, ',');
        g_test[1] = test ? (f32)atof(test + 1) : 0;
    }
    LOG("mouse_look: the mouse turns the play camera itself%s\n", test ? " (a test motion each update)" : "");
}
