/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The title ID a main.npdm gives its program for one not loaded (snx_npdm_title), as the launcher reads an older
   game's, and the 16 hex digits a game's folder and files are named by (snx_title_text) */
#include "common/utilities/common.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* 0100DA801624E000, without C99's long long constants */
#define TUNIC (((SnxU64)0x0100DA80UL << 32) | 0x1624E000UL)

int main(void) {
    unsigned char meta[0xa0];
    char id[17];
    int i;
    memset(meta, 0, sizeof meta);
    memcpy(meta, "META", 4);
    meta[0x70] = 0x80; /* the ACI0 at 0x80, the program's ID 16 bytes in */
    memcpy(meta + 0x80, "ACI0", 4);
    for (i = 0; i < 8; ++i) meta[0x90 + i] = (unsigned char)(TUNIC >> (8 * i));
    assert(snx_npdm_title(meta, sizeof meta) == TUNIC);
    assert(snx_title_text(snx_npdm_title(meta, sizeof meta), id) && !strcmp(id, "0100DA801624E000"));
    /* none of the program's own: no NPDM, a short one, no META, an ACI0 past its end or not named so */
    assert(snx_npdm_title(NULL, 0) == SNX_NPDM_DEFAULT_TITLE && snx_npdm_title(meta, 0x7f) == SNX_NPDM_DEFAULT_TITLE);
    assert(snx_npdm_title(meta, 0x97) == SNX_NPDM_DEFAULT_TITLE); /* its ID cut */
    meta[0x70] = 0xf0;
    assert(snx_npdm_title(meta, sizeof meta) == SNX_NPDM_DEFAULT_TITLE);
    meta[0x70] = 0x80;
    meta[0x80] = 'X';
    assert(snx_npdm_title(meta, sizeof meta) == SNX_NPDM_DEFAULT_TITLE);
    meta[0x80] = 'A';
    meta[0] = 'X';
    assert(snx_npdm_title(meta, sizeof meta) == SNX_NPDM_DEFAULT_TITLE);
    /* an ID of none of its own names nothing: the game keeps its name */
    assert(!snx_title_text(SNX_NPDM_DEFAULT_TITLE, id) && !id[0] && !snx_title_text(0, id) && !id[0]);
    assert(snx_title_text((SnxU64)0xABUL, id) && !strcmp(id, "00000000000000AB"));
    puts("PASS a main.npdm's title ID and the folder named by it");
    return 0;
}
