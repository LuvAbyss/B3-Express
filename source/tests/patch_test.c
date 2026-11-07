/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Code-patch mods (nx/loader/patch.c): IPS, IPS32 and IPSwitch text patches on a made-up module, and mods.txt */
#include "nx/loader/patch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(v)                                                                                                       \
    do {                                                                                                               \
        ++checks;                                                                                                      \
        if (!(v)) {                                                                                                    \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #v);                                                    \
            exit(1);                                                                                                   \
        }                                                                                                              \
    } while (0)

static unsigned char image[0x3000], build_id[32];
static SnxPatchTarget target = {image, sizeof image, build_id, 0, 0x1000};
static SnxPatchStats stats;
static char error[300];

static int apply(const char* name, const char* data, size_t size) {
    return snx_patch_apply(name, (const unsigned char*)data, size, &target, &stats, error, sizeof error);
}
static int text(const char* name, const char* data) { return apply(name, data, strlen(data)); }
static void reset(void) {
    memset(image, 0, sizeof image);
    memset(&stats, 0, sizeof stats);
    stats.digest = SNX_PATCH_DIGEST0;
    error[0] = 0;
}

int main(void) {
    unsigned i;
    uint64_t first;
    for (i = 0; i < 32; ++i) build_id[i] = i < 20 ? (unsigned char)(0xa0 + i) : 0;
    CHECK(snx_patch_kind("x.IPS") && snx_patch_kind("y.pchtxt") && !snx_patch_kind("readme.txt"));

    /* IPS, named by the build ID (20 bytes of it in any case): a record and a run, offsets with the 0x100 header */
    reset();
    {
        static const char ips[] = "PATCH\x00\x01\x10\x00\x02\xab\xcd\x00\x12\x00\x00\x00\x00\x03\x5a"
                                  "\x00\x00\x10\x00\x01\xee" /* inside the header: skipped */
                                  "EOF";
        CHECK(apply("A0A1A2A3A4A5A6A7A8A9AAABACADAEAFB0B1B2B3.ips", ips, sizeof ips - 1) == 1);
        CHECK(image[0x10] == 0xab && image[0x11] == 0xcd && image[0x10ff] == 0 && image[0x1100] == 0x5a &&
              image[0x1102] == 0x5a && image[0x1103] == 0);
        CHECK(stats.records == 2 && stats.bytes == 5 && stats.text_bytes == 2 && stats.digest != SNX_PATCH_DIGEST0);
        first = stats.digest;
        /* another module's build, or too short a name: not this module's */
        CHECK(apply("A0A1A2A3A4A5A6A8.ips", ips, sizeof ips - 1) == 0);
        CHECK(apply("A0A1A2.ips", ips, sizeof ips - 1) == 0);
        /* the same patch again gives the same digest from the start */
        reset();
        CHECK(apply("a0a1a2a3a4a5a6a7.ips", ips, sizeof ips - 1) == 1 && stats.digest == first);
    }
    /* IPS32 with an offset past 16 MB would be outside; within the module it lands */
    reset();
    {
        static const char ips32[] = "IPS32\x00\x00\x21\x00\x00\x01\x77"
                                    "EEOF";
        static const char past[] = "IPS32\x00\x00\x31\x00\x00\x01\x77"
                                   "EEOF";
        static const char cut[] = "PATCH\x00\x01\x10\x00\x09\xab";
        CHECK(apply("a0a1a2a3a4a5a6a7.ips", ips32, sizeof ips32 - 1) == 1 && image[0x2000] == 0x77);
        CHECK(apply("a0a1a2a3a4a5a6a7.ips", past, sizeof past - 1) == -1 && strstr(error, "past the module"));
        CHECK(apply("a0a1a2a3a4a5a6a7.ips", cut, sizeof cut - 1) == -1);
        CHECK(apply("a0a1a2a3a4a5a6a7.ips", "NOTIPS", 6) == -1);
    }

    /* IPSwitch text: comments, offset_shift, enabled and disabled blocks, a string with a NUL, @stop */
    reset();
    CHECK(text("wide.pchtxt",
               "# 21:9\r\n@nsobid-A0A1A2A3A4A5A6A7A8A9AAABACADAEAFB0B1B2B3\r\n"
               "@flag offset_shift 0x100\r\n// a comment\r\n@enabled\r\n"
               "00000020 1F2003D5 // nop\r\n00001000 \"hi\\n\"\r\n"
               "@disabled\r\n00000030 FFFFFFFF\r\n@enabled\r\n00000040 0A\r\n@stop\r\n00000050 BB\r\n") == 1);
    CHECK(image[0x20] == 0x1f && image[0x21] == 0x20 && image[0x22] == 0x03 && image[0x23] == 0xd5);
    CHECK(!memcmp(image + 0x1000, "hi\n", 4) && image[0x30] == 0 && image[0x40] == 0x0a && image[0x50] == 0);
    CHECK(stats.records == 3 && stats.text_bytes == 5);
    /* no offset_shift: offsets count with the header, as IPS */
    reset();
    CHECK(text("p.pchtxt", "@nsobid-a0a1a2a3a4a5a6a7\n@enabled\n00000110 99\n") == 1 && image[0x10] == 0x99);
    /* another build, a patch before the build is named, no build at all, a bad value */
    CHECK(text("p.pchtxt", "@nsobid-b0b1b2b3b4b5b6b7\n00000110 99\n") == 0);
    CHECK(text("p.pchtxt", "00000110 99\n@nsobid-a0a1a2a3a4a5a6a7\n") == -1);
    CHECK(text("p.pchtxt", "# nothing\n") == -1 && strstr(error, "no @nsobid"));
    CHECK(text("p.pchtxt", "@nsobid-a0a1a2a3a4a5a6a7\n00000110 9G\n") == -1);
    CHECK(text("p.pchtxt", "@nsobid-a0a1a2a3a4a5a6a7\n00000110 \"open\n") == -1);

    /* mods.txt as the RomFS overlay reads it */
    {
        char list[] = "# order\r\n one \r\n../x\r\ntwo # c\r\none\r\n.\r\na:b\r\n\r\nthree";
        char* names[8];
        size_t n = snx_patch_mod_names(list, names, 8);
        CHECK(n == 3 && !strcmp(names[0], "one") && !strcmp(names[1], "two") && !strcmp(names[2], "three"));
    }
    printf("Code-patch mods: %u checks passed\n", checks);
    return 0;
}
