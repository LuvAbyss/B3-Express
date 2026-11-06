/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* BC7 encoding on the CPU (bc7.c): the texture pack's encoder, for the game's ASTC textures decoded to RGBA8 */
#ifndef SNX_BC7_H
#define SNX_BC7_H
#include "common/cpu/aarch64/cpu.h"

/* one 4x4 block of RGBA8 texels (row after row) into 16 bytes of BC7 */
void bc7_encode_block(const u8 px[16][4], u8 out[16]);
/* a w x h RGBA8 image (w * 4 bytes a row) into rows of BC7 blocks, ((w + 3) / 4) * ((h + 3) / 4) * 16 bytes; a block
   past the image's edge repeats its last row and column */
void bc7_encode_image(const u8* rgba, u32 w, u32 h, u8* dst);

#endif
