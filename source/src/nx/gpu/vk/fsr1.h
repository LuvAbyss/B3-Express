/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* AMD FSR 1 on the graphics queue, for the host path's final picture */
#ifndef SNX_VK_FSR1_H
#define SNX_VK_FSR1_H
#include "nx/gpu/vk/vk.h"
int vk_fsr1_ready(void); /* its shaders are built in */
/* src (sw x sh) upscaled into tmp and sharpened into dst, both dw x dh RGBA8 storage images. All three in GENERAL, and
   no rendering pass open. sharpness in stops, 0 the sharpest. Under vk_lock */
void vk_fsr1_run(VkCommandBuffer cb, VkImageView src, u32 sw, u32 sh, VkImageView tmp, VkImageView dst, u32 dw, u32 dh,
                 float sharpness);
#endif
