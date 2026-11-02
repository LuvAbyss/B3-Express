/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/utilities/pad_map.h"
#include <stdio.h>
#include <string.h>

static const char* const KEYS[SNX_PAD_CONTROLS] = {"south", "east", "west", "north", "lb", "rb",   "lt",   "rt",
                                                   "ls",    "rs",   "back", "start", "up", "down", "left", "right"};
/* the font's (CP437) arrows for the D-pad */
static const char* const NAMES[SNX_PAD_CONTROLS] = {"A",  "B",  "X",    "Y",     "LB",   "RB",   "LT",   "RT",
                                                    "LS", "RS", "Back", "Start", "\x18", "\x19", "\x1B", "\x1A"};
static const char* const LABELS[] = {NULL, "A", "B", "X", "Y", "Cross", "Circle", "Square", "Triangle"};
static const char* const BUTTONS[SNX_PAD_BUTTONS] = {"A",  "B",  "X",    "Y",     "L3",   "R3", "L",     "R",
                                                     "ZL", "ZR", "Plus", "Minus", "Left", "Up", "Right", "Down"};
/* where the console's controller has them: A right, B bottom, X top, Y left */
static const signed char POSITION[SNX_PAD_CONTROLS] = {1, 0, 3, 2, 6, 7, 8, 9, 4, 5, 11, 10, 13, 15, 12, 14};

const char* snx_pad_control_key(int control) { return control >= 0 && control < SNX_PAD_CONTROLS ? KEYS[control] : ""; }
const char* snx_pad_control_name(int control, const int* labels) {
    if (control < 0 || control >= SNX_PAD_CONTROLS) return "";
    if (labels && control <= SNX_PAD_NORTH && labels[control] > SNX_PAD_LABEL_NONE &&
        labels[control] <= SNX_PAD_LABEL_TRIANGLE)
        return LABELS[labels[control]];
    return NAMES[control];
}
const char* snx_pad_button_name(int bit) { return bit >= 0 && bit < SNX_PAD_BUTTONS ? BUTTONS[bit] : "None"; }

void snx_pad_map_position(SnxPadMap* map) { memcpy(map->bit, POSITION, sizeof map->bit); }
void snx_pad_map_swap_ab(SnxPadMap* map) {
    int c;
    for (c = 0; c < SNX_PAD_CONTROLS; ++c)
        if (map->bit[c] == 0 || map->bit[c] == 1) map->bit[c] = (signed char)(1 - map->bit[c]); /* A is 0, B 1 */
}
void snx_pad_map_unset(SnxPadMap* map) { memset(map->bit, SNX_PAD_UNSET, sizeof map->bit); }
void snx_pad_map_fill(SnxPadMap* map, const SnxPadMap* from) {
    int c;
    for (c = 0; c < SNX_PAD_CONTROLS; ++c)
        if (map->bit[c] == SNX_PAD_UNSET) map->bit[c] = from->bit[c];
}
/* the index of the word of n letters at s in names (count of them), any case; -1 when none */
static int lookup(const char* const* names, int count, const char* s, size_t n) {
    int i;
    size_t k;
    for (i = 0; i < count; ++i) {
        if (strlen(names[i]) != n) continue;
        for (k = 0; k < n && (names[i][k] | 0x20) == (s[k] | 0x20); ++k) {}
        if (k == n) return i;
    }
    return -1;
}
int snx_pad_map_read(SnxPadMap* map, const char* text, char* error, size_t size) {
    SnxPadMap read = *map;
    int named[SNX_PAD_CONTROLS] = {0};
    const char* at = text;
    while (*at) {
        size_t n = strcspn(at, " \t"), key = strcspn(at, ":");
        int control = key < n ? lookup(KEYS, SNX_PAD_CONTROLS, at, key) : -1, bit = -2;
        if (!n) {
            ++at;
            continue;
        }
        if (control >= 0 && !named[control]) {
            static const char* const NONE[] = {"None"};
            bit = lookup(BUTTONS, SNX_PAD_BUTTONS, at + key + 1, n - key - 1);
            if (bit < 0) bit = lookup(NONE, 1, at + key + 1, n - key - 1) ? -2 : SNX_PAD_NONE;
        }
        if (bit < SNX_PAD_NONE) {
            if (size) snprintf(error, size, "%.*s", (int)n, at);
            return -1;
        }
        named[control] = 1;
        read.bit[control] = (signed char)bit;
        at += n;
    }
    *map = read;
    return 0;
}
void snx_pad_map_write(const SnxPadMap* map, char* out, size_t size) {
    size_t at = 0;
    int c;
    if (size) out[0] = 0;
    for (c = 0; c < SNX_PAD_CONTROLS && at < size; ++c) {
        int n;
        if (map->bit[c] == SNX_PAD_UNSET) continue;
        n = snprintf(out + at, size - at, "%s%s:%s", at ? " " : "", KEYS[c], snx_pad_button_name(map->bit[c]));
        if (n < 0) break;
        at += (size_t)n;
    }
}
int snx_pad_map_find(const SnxPadMap* map, int bit) {
    int c;
    for (c = 0; c < SNX_PAD_CONTROLS; ++c)
        if (map->bit[c] == bit) return c;
    return -1;
}
