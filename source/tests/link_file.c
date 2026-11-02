/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* snx_link_file, a second name of a file (the Windows DLLs in each game's folder): never over a file that is there,
   nor from one that is not */
#include "common/utilities/common.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    char *dir = snx_temp_dir(NULL), *a, *b, *other;
    unsigned char* data;
    size_t size;
    assert(dir);
    a = snx_path_join(dir, "runtime.dll");
    b = snx_path_join(dir, "game.dll");
    other = snx_path_join(dir, "other.dll");
    assert(!snx_write_file(a, "library", 7) && !snx_write_file(other, "older", 5));
    assert(!snx_link_file(a, b));
    data = snx_read_file(b, &size);
    assert(data && size == 7 && !memcmp(data, "library", 7));
    free(data);
    assert(snx_link_file(a, other)); /* there already: left as it was, for the caller to copy over */
    data = snx_read_file(other, &size);
    assert(data && size == 5 && !memcmp(data, "older", 5));
    free(data);
    assert(!snx_remove_tree(a));
    assert(snx_link_file(a, other) && snx_is_file(b)); /* none at a; b stays, a name of its own */
    assert(!snx_remove_tree(dir));
    free(a);
    free(b);
    free(other);
    free(dir);
    puts("link_file: a second name, never over a file, never from none");
    return 0;
}
