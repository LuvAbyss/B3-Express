/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The controllers as the frontend sees them, which hid turns into the samples in its shared memory */
#ifndef SNX_HID_H
#define SNX_HID_H
#include "common/cpu/aarch64/cpu.h"
#include "nx/hle/services/hid_touch.h" /* TouchState */
#include "nx/runtime/platform/plat.h"  /* PMutex */

/* One motion sensor, in the Switch controller's frame: acceleration in g, angular velocity in rad/s */
typedef struct {
    float acceleration[3];
    float angular_velocity[3];
    int valid;
} PadMotion;

/* the physical controller behind a player */
typedef enum { SNX_PAD_FULL_KEY = 0, SNX_PAD_JOY_DUAL, SNX_PAD_JOY_LEFT, SNX_PAD_JOY_RIGHT } PadKind;

typedef struct {
    u64 buttons;
    i32 lx, ly, rx, ry;
    int connected; /* player 1 stays connected, for the keyboard and scripted input */
    PadKind kind;
    u32 source_id;       /* the SDL instance id, which changes when the controller is replaced */
    PadMotion motion[2]; /* the left (or only) sensor, and the right one when there is one */
    /* vibration the game asked for, left and right: amp_low in bits 0-15, amp_high in bits 16-31. Game threads write
       it and the frontend plays it */
    u32 rumble[2];
} PadState;

/* The frontend publishes these and hid samples them, both under g_pad_input_lock */
extern PadState g_pad;          /* player 1 */
extern PadState g_pad_extra[7]; /* players 2 to 8, in Npad id order */
extern TouchState g_touch;
extern PMutex g_pad_input_lock;

void hid_update(void); /* the next samples in shared memory, touch screen included */
/* the number of connected players, and the Npad id of the first in *first_id */
int hid_connected_players(u32* first_id);
#endif
