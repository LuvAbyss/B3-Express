/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the B3-Express contributors */
/* PlayStation / Xbox button prompts painted over the game's atlas (prompts.c) */
#ifndef SNX_VK_PROMPTS_H
#define SNX_VK_PROMPTS_H
#include "nx/gpu/vk/vk.h"

/* texture.c, a texture made from memory: remembers it when it is the game's button atlas, and paints it */
void vk_prompts_texture(u64 gva, const VkFmt* f, u32 w, u32 h, u32 bh_log2);
/* once a present: guest memory gets the set gamepad_prompts asks for */
void vk_prompts_frame(void);
/* the parts, for tests: one ASTC block from 4 x 4 RGBA texels (row stride in texels); a set painted over an atlas */
void vk_prompts_astc_block(const uint8_t* rgba, int stride, uint8_t* out);
void vk_prompts_paint(uint8_t* mem, int family, int by_letter);

#endif
