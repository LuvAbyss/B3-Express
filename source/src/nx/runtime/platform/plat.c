/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The log (<game>.log in the state folder, and stderr) and fatal errors */
#include "nx/runtime/platform/log.h"
#include "nx/runtime/loader.h"
#include "nx/runtime/main.h"
#include "nx/runtime/platform/backend.h"
#include "nx/runtime/platform/plat.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#if !defined(_WIN32)
#include <unistd.h>
#endif

int g_verbose = 1;
__thread FILE* tls_log_redirect;
__thread void* tls_thread;

#define LINE_MAX_ 2048
#define EARLY_MAX 4096

/* Initialised statically: threads log from the first moments of the run, and a lazy init would race */
static PMutex g_log_lock = PMUTEX_INIT;
/* Everything below is written under g_log_lock. The few fields crash mode reads without it are accessed atomically */
static FILE* g_log;        /* atomic for crash mode's reads */
#if !defined(_WIN32)
static int g_log_fd = -1; /* g_log's descriptor, for crash mode's write() */
#endif
static int g_log_gave_up;  /* the log could not be created: the console has it all */
static char g_early[EARLY_MAX]; /* lines from before the log exists, written at its top */
static size_t g_early_len;
static int g_early_full; /* a line did not fit: no more are kept, so the saved part has no gap */
static u64 g_log_t0;     /* plat_ns() of the first line, atomic */
/* written by every line, apart from the flag every line reads */
static int g_mid_line __attribute__((aligned(64))); /* the last text written did not end its line, atomic */
static int g_crashing __attribute__((aligned(64))); /* log_crash_mode was called, atomic */

void log_crash_mode(void) {
    __atomic_store_n(&g_crashing, 1, __ATOMIC_SEQ_CST);
    tls_log_redirect = NULL;
}

/* The log is made once the game's code module is loaded (g_mod), so a run that never gets that far (a mistyped
   --game, a dump given instead of a game) leaves no log behind */
static void log_create_nl(void) {
    char path[1400];
    FILE* file;
    if (g_log || g_log_gave_up || !g_mod) return;
    snprintf(path, sizeof path, "%s%s.log", g_statedir, g_exename);
    file = fopen(path, "w");
    if (!file) {
        g_log_gave_up = 1;
        fprintf(stderr, "Could not create the log %s, so it only goes to the console.\n", path);
    } else {
        if (g_early_len) fwrite(g_early, 1, g_early_len, file);
        fflush(file);
#if !defined(_WIN32)
        __atomic_store_n(&g_log_fd, fileno(file), __ATOMIC_RELEASE);
#endif
        __atomic_store_n(&g_log, file, __ATOMIC_RELEASE);
    }
    g_early_full = 1; /* either way nothing more is kept */
}

/* the "[seconds] " stamp when text starts a line, then the text, into out */
static size_t log_compose(char* out, size_t cap, const char* text) {
    size_t n = 0, len = strlen(text);
    u64 t0 = __atomic_load_n(&g_log_t0, __ATOMIC_RELAXED);
    if (!__atomic_load_n(&g_mid_line, __ATOMIC_RELAXED)) {
        int w;
        if (!t0) {
            t0 = plat_ns();
            __atomic_store_n(&g_log_t0, t0 ? t0 : 1, __ATOMIC_RELAXED);
        }
        w = snprintf(out, cap, "[%7.2f] ", (double)(plat_ns() - t0) / 1e9);
        n = w > 0 && (size_t)w < cap ? (size_t)w : 0;
    }
    if (len > cap - 1 - n) len = cap - 1 - n;
    memcpy(out + n, text, len);
    n += len;
    out[n] = 0;
    if (len) __atomic_store_n(&g_mid_line, text[len - 1] != '\n', __ATOMIC_RELAXED);
    return n;
}

/* Crash mode: no lock and no stdio buffers (the crashed thread may own either), straight to the descriptors */
static void log_crash_write(const char* text) {
    char out[LINE_MAX_ + 32];
    size_t n = log_compose(out, sizeof out, text);
#if defined(_WIN32)
    /* the report runs in an exception filter, on the thread that crashed; stdio's locks are recursive */
    FILE* file = __atomic_load_n(&g_log, __ATOMIC_ACQUIRE);
    (void)n;
    if (file) {
        fputs(out, file);
        fflush(file);
    }
    fputs(text, stderr);
    fflush(stderr);
#else
    int fd = __atomic_load_n(&g_log_fd, __ATOMIC_ACQUIRE);
    ssize_t r;
    if (fd >= 0) r = write(fd, out, n);
    r = write(2, text, strlen(text));
    (void)r;
#endif
}

void logf_(int lvl, const char* fmt, ...) {
    char text[LINE_MAX_], out[LINE_MAX_ + 32];
    va_list ap;
    int w;
    size_t n;
    va_start(ap, fmt);
    w = vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    if (w < 0) return;
    /* a cut line still ends with its newline, so the next one starts on its own */
    if ((size_t)w >= sizeof text) text[sizeof text - 2] = '\n';
    if (tls_log_redirect) {
        fputs(text, tls_log_redirect);
        return;
    }
    if (__atomic_load_n(&g_crashing, __ATOMIC_SEQ_CST)) {
        log_crash_write(text);
        return;
    }
    pm_lock(&g_log_lock);
    log_create_nl();
    n = log_compose(out, sizeof out, text);
    if (g_log) {
        fputs(out, g_log);
        fflush(g_log);
        if (lvl <= 1 || g_verbose >= 2) {
            fputs(text, stderr); /* the console gets the line without its time stamp, which only the log file keeps */
            fflush(stderr);
        }
    } else {
        fputs(text, stderr);
        fflush(stderr);
        if (!g_early_full && !g_log_gave_up) {
            if (n <= sizeof g_early - g_early_len) {
                memcpy(g_early + g_early_len, out, n);
                g_early_len += n;
            } else
                g_early_full = 1;
        }
    }
    pm_unlock(&g_log_lock);
}

void fatal(const char* fmt, ...) {
    char msg[LINE_MAX_];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    tls_log_redirect = NULL; /* a failure while writing threads.txt belongs in the log */
    logf_(0, "FATAL: %s\n", msg);
#if !defined(_WIN32)
    /* from the crash handler: a box or a flush could wait on a lock the crashed code holds */
    if (__atomic_load_n(&g_crashing, __ATOMIC_SEQ_CST)) _exit(1);
#endif
    /* everywhere, since a player started from a desktop entry sees no console and no log */
    win_fatal_box(msg);
    plat_exit_now(1);
}
