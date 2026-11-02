/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The button map screen, the launcher's (Settings > Controllers) and the game's menu's: a box for each control down
   either side with the console button it sends, the pad drawn between them (or, with none, why and what to do), and a
   bar at the bottom with what the focus does. This holds what the screen shows and what a key does to it, and lays it
   out as boxes, texts and shapes that each side draws its own way, none over another. No SDL: the caller tells it the
   pad's labels and what is held */
#ifndef SNX_PAD_SCREEN_H
#define SNX_PAD_SCREEN_H
#include "common/utilities/pad_map.h"

typedef struct {
    SnxPadMap own;  /* the player's map, SNX_PAD_UNSET where a control sends the button in its place */
    int swap_ab;    /* A and B swapped over it (gamepad_swap_ab) */
    int labels[4];  /* the pad's face labels (south, east, west, north), SNX_PAD_LABEL_NONE for none */
    int symmetric;  /* the sticks side by side under the D-pad and the buttons (a PlayStation pad's) */
    char pad[48];   /* the pad's name, empty for none */
    char note[256]; /* with no pad, why: none plugged in when empty */
    unsigned held;  /* the controls held, a bit each */
    int focus;      /* the control focused */
    int listening;  /* the next control pressed takes the focused one's button */
} SnxPadScreen;
/* screen with no map of the player's, its first control focused */
void snx_pad_screen_start(SnxPadScreen* screen);
/* the map shown: the button in each control's place, the player's over that, A and B swapped when asked */
void snx_pad_screen_map(const SnxPadScreen* screen, SnxPadMap* map);
/* what a key or a press did, for the caller to keep */
enum { SNX_PAD_DID_NOTHING, SNX_PAD_DID_MAP, SNX_PAD_DID_LEAVE };
enum { SNX_PAD_KEY_UP, SNX_PAD_KEY_DOWN, SNX_PAD_KEY_LEFT, SNX_PAD_KEY_RIGHT, SNX_PAD_KEY_SELECT, SNX_PAD_KEY_BACK };
/* Up/Down move round the controls, Left/Right change the focused one's button and Select waits for a press; Back stops
   waiting, then leaves. A change makes the whole map the player's (SNX_PAD_DID_MAP: own to keep) */
int snx_pad_screen_key(SnxPadScreen* screen, int key);
/* a control of the pad pressed: while waiting, it takes the focused control's button and gives its own */
int snx_pad_screen_press(SnxPadScreen* screen, int control);
/* a tap or a click on what snx_pad_screen_hit found: a control focused, then waited for */
int snx_pad_screen_tap(SnxPadScreen* screen, int id);

/* the screen laid out: kinds of shape, and how each is drawn */
enum {
    SNX_PAD_SHAPE_BOX,  /* a panel: a control's box, the bar */
    SNX_PAD_SHAPE_TEXT, /* text from x, y (its top left), in the font's (CP437) glyphs of w x h pixels */
    SNX_PAD_SHAPE_BAR,  /* a part of the pad drawn: a shoulder, a D-pad arm, Back and Start */
    SNX_PAD_SHAPE_DISC  /* a round part: a stick, a face button, in the box x, y, w, h */
};
enum {
    SNX_PAD_STYLE_PLAIN, /* the page's ink */
    SNX_PAD_STYLE_SOFT,  /* the softer ink of hints */
    SNX_PAD_STYLE_FOCUS, /* the focus: a box filled, its text in the page's colour */
    SNX_PAD_STYLE_LIT,   /* held on the pad now */
    SNX_PAD_STYLE_NOTE   /* why there is no pad: the failure's colour */
};
typedef struct {
    unsigned char kind, style;
    short id; /* what a tap on it means: a control, -1 for nothing */
    float x, y, w, h;
    const char* text; /* a text's, in the drawing's own buffer */
} SnxPadShape;
#define SNX_PAD_SHAPES 128
typedef struct {
    SnxPadShape shape[SNX_PAD_SHAPES];
    int n;
    char text[2048];
    int used;
} SnxPadDrawing;
/* screen on the area x, y, w, h in pixels, its text in glyphs of cw x ch: wide, the controls' left boxes, the pad and
   their right boxes side by side; narrow, the pad over the boxes in two columns; too small for the pad, the boxes
   alone */
void snx_pad_screen_lay(const SnxPadScreen* screen, float x, float y, float w, float h, float cw, float ch,
                        SnxPadDrawing* drawing);
/* the id of what is at x, y, -1 for none */
int snx_pad_screen_hit(const SnxPadDrawing* drawing, float x, float y);
#endif
