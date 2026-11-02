/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Horizon results and the helpers that return them. A result is a 32-bit value, 0 for success, otherwise a module in
   bits 0-8 and a description from bit 9. The helpers are the idiom of Atmosphère's R_ macros, written anew in C90:
   each statement macro is one statement (do { } while (0)), safe as the body of an if or else, and evaluates each
   argument at most once. Self-contained, so strict C90 code can use it */
#ifndef SNX_RESULT_H
#define SNX_RESULT_H

#define RC(mod, desc) ((mod) | ((desc) << 9))
#define RC_OK         0

#define R_SUCCEEDED(rc) ((rc) == 0)
#define R_FAILED(rc)    ((rc) != 0)

/* return success */
#define R_SUCCEED()                                                                                                    \
    do {                                                                                                               \
        return RC_OK;                                                                                                  \
    } while (0)
/* return rc */
#define R_THROW(rc)                                                                                                    \
    do {                                                                                                               \
        return (rc);                                                                                                   \
    } while (0)
/* return rc when cond is false (rc is evaluated only then) */
#define R_UNLESS(cond, rc)                                                                                             \
    do {                                                                                                               \
        if (!(cond)) return (rc);                                                                                      \
    } while (0)
/* evaluate expr once and return its result when it failed. The temporary is 32 bits, as a result is, and converts
   back to the function's int or u32 */
#define R_TRY(expr)                                                                                                    \
    do {                                                                                                               \
        unsigned int snx_r_try_result_ = (unsigned int)(expr);                                                         \
        if (snx_r_try_result_ != 0) return snx_r_try_result_;                                                          \
    } while (0)
#endif
