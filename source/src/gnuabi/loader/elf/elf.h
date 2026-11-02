/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_GNU_ELF_H
#define SNX_GNU_ELF_H
#include "gnuabi/memory/memory.h"
typedef struct GnuElfRange {
    uint64_t address;
    const unsigned char* data;
    size_t size;
} GnuElfRange;
typedef struct GnuElf {
    unsigned char* image;
    size_t size;
    uint64_t entry, bias, phdr, image_end;
    uint16_t phnum, phentsize;
    unsigned stack_permissions;
    unsigned bits; /* 32 or 64 */
    int thumb;     /* AArch32 entry in Thumb state (entry bit 0) */
    GnuElfRange* code;
    size_t code_count;
    char interp[256]; /* PT_INTERP: the dynamic linker's guest path, "" for a static program */
} GnuElf;
/* ET_DYN load addresses: a position-independent program, and the dynamic linker beside it */
#define GNU_ELF_PROGRAM_BIAS UINT64_C(0x10000000)
#define GNU_ELF_INTERP_BIAS  UINT64_C(0x38000000)
int gnu_elf_load(GnuElf* elf, GnuMemory* memory, const char* path, char* error, size_t error_capacity);
int gnu_elf_load_image(GnuElf* elf, GnuMemory* memory, const void* image, size_t size, char* error,
                       size_t error_capacity);
/* program 0: the dynamic linker, at bias when it is ET_DYN, leaving brk and the program headers to the program */
int gnu_elf_load_at(GnuElf* elf, GnuMemory* memory, const void* image, size_t size, uint64_t bias, int program,
                    char* error, size_t error_capacity);
void gnu_elf_destroy(GnuElf* elf);
#endif
