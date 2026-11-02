/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_CTR_COMPILE_H
#define SNX_CTR_COMPILE_H
#include <stddef.h>
/* Compiles the extracted title's ARM11 code into <extracted>/native/code.so, replacing an
   earlier module once the new one is complete. 0 on success, 3 for an unsupported image, else 1, with error filled */
int snx_ctr_aot_compile(const char* extracted, char* error, size_t error_size);
#endif
