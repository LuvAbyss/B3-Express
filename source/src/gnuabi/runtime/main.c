/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "gnuabi/process/process.h"
#include "gnuabi/runtime/execution.h"
#include "common/utilities/common.h"
#include "common/cpu/recompiler/snx_recompiler.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#else
#include <pthread.h>
#endif

/* Guest calls are host calls in the generated code, so the guest's call depth is the host thread's: the guest runs on
   a thread of its own with a stack of this size, which the host only commits as it is used */
#define GUEST_STACK ((size_t)1 << 30)

static const char usage[] =
    "Usage: scarletnx-gnuabi [options] [--] ELF [arguments...]\n"
    "Runs Linux AArch64 or AArch32 (A32/Thumb) programs, static or dynamically linked, through the ScarletNX\n"
    "recompiler.\n"
    "\n"
    "  -L DIR, --sysroot=DIR  The guest's root: a guest path names DIR's file when DIR has it, the host's\n"
    "                         otherwise, as with QEMU's -L. The default is linux/ of the data folder games/ is\n"
    "                         in; SNX_LINUX_SYSROOT names another\n"
    "  --isolated             No host files: /tmp in memory, the program alone as /program\n"
    "  --jit                  Compile the guest's code as it runs (the default)\n"
    "  --aot                  Compile the program before it starts with the host compiler, and what it maps\n"
    "                         later as it runs\n"
    "  --cc=COMPILER          The host compiler of --aot\n"
    "  --jit-cache=DIR        Where compiled code is kept between runs (default: the user's cache folder);\n"
    "  --no-jit-cache         keep none\n"
    "  --jit-opt=LEVEL        The JIT's LLVM optimization level: 0, 1, 2 (the default), 3, s or z\n"
    "  --verbose-jit          Say what the JIT compiles\n"
    "  -E VAR=VALUE, -U VAR   Set or remove a variable of the guest's environment (the host's otherwise)\n"
    "  -0 ARGV0               The program's argv[0] (default: ELF)\n";

static int run_status;
static GnuProcess* process;

/* the guest's thread: the process ends here, in a forked child too, where it is the only thread */
static void guest_main(void) {
    gnu_start_sampler(process);
    run_status = gnu_runtime_run(process);
    if (process->faulted && process->diagnostic[0]) fprintf(stderr, "scarletnx-gnuabi: %s\n", process->diagnostic);
    gnu_spawn_exit(process);
}
#ifdef _WIN32
static DWORD WINAPI guest_thread(LPVOID unused) {
    (void)unused;
    guest_main();
    return 0;
}
#else
static void* guest_thread(void* unused) {
    (void)unused;
    guest_main();
    return NULL;
}
#endif

/* the environment: the host's, with -E and -U applied in order */
static char** environment(char** changes, size_t change_count) {
#ifdef _WIN32
    extern char** _environ;
    char** host = _environ;
#else
    extern char** environ;
    char** host = environ;
#endif
    size_t count = 0, i, j;
    char** env;
    while (host && host[count]) ++count;
    env = calloc(count + change_count + 1, sizeof *env);
    if (!env) return NULL;
    for (i = 0; i < count; ++i) env[i] = host[i];
    for (j = 0; j < change_count; ++j) {
        const char* change = changes[j];
        int set = change[0] == '+';
        const char* name = change + 1;
        size_t n = set ? (size_t)(strchr(name, '=') - name) : strlen(name);
        for (i = 0; i < count; ++i)
            if (!strncmp(env[i], name, n) && env[i][n] == '=') break;
        if (set) {
            if (i == count) ++count;
            env[i] = (char*)name;
        } else if (i < count) {
            memmove(env + i, env + i + 1, (count - i) * sizeof *env);
            --count;
        }
    }
    env[count] = NULL;
    return env;
}

/* linux/ of the data folder games/ and work/ are in (snx_data_dir), as the 3DS runtime finds work/3ds */
static char* default_sysroot(const char* argv0) {
    const char* named = getenv("SNX_LINUX_SYSROOT");
    char *exe, *dir, *data, *root, *checkout;
    if (named && named[0]) return snx_absolute(named);
    exe = snx_executable(argv0);
    dir = exe ? snx_dirname(exe) : NULL;
    data = snx_data_dir(dir);
    root = data ? snx_path_join(data, "linux") : NULL;
    checkout = snx_checkout(SNX_SOURCE_DIR, SNX_BUILD_DIR, dir);
    if (checkout && data) snx_checkout_left(checkout, data);
    free(checkout);
    free(exe);
    free(dir);
    free(data);
    return root;
}

int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    const char *cc = NULL, *argv0 = NULL, *cache_option = NULL, *opt = "2";
    char *sysroot = NULL, *cache = NULL;
    char** changes = calloc((size_t)argc + 1, sizeof *changes);
    size_t change_count = 0;
    /* a process another copy of this runtime started (Windows' execve): its descriptors and working folder */
    int adopted_fd[64], adopted_count = 0;
    unsigned long long adopted_host[64];
    const char* cwd = NULL;
    int first = 1, mode = GNU_JIT, isolated = 0, no_cache = 0, verbose = 0;
    if (!changes) return 2;
    while (first < argc && argv[first][0] == '-') {
        const char* option = argv[first];
        const char* value = first + 1 < argc ? argv[first + 1] : NULL;
        if (!strcmp(option, "--help") || !strcmp(option, "-h")) {
            fputs(usage, stdout);
            return 0;
        }
        if (!strcmp(option, "--")) {
            first++;
            break;
        }
        if (!strncmp(option, "--cc=", 5)) {
            cc = option + 5;
            mode = GNU_AOT;
        } else if (!strncmp(option, "--sysroot=", 10)) {
            free(sysroot);
            sysroot = snx_absolute(option + 10);
        } else if (!strcmp(option, "-L") && value) {
            free(sysroot);
            sysroot = snx_absolute(value);
            first++;
        } else if (!strcmp(option, "--isolated"))
            isolated = 1;
        else if (!strcmp(option, "--jit"))
            mode = GNU_JIT;
        else if (!strcmp(option, "--aot"))
            mode = GNU_AOT;
        else if (!strncmp(option, "--jit-cache=", 12))
            cache_option = option + 12;
        else if (!strcmp(option, "--no-jit-cache"))
            no_cache = 1;
        else if (!strncmp(option, "--jit-opt=", 10))
            opt = option + 10;
        else if (!strcmp(option, "--verbose-jit"))
            verbose = 1;
        else if (!strcmp(option, "-E") && value && strchr(value, '=')) {
            char* change = snx_format("+%s", value);
            changes[change_count++] = change;
            first++;
        } else if (!strcmp(option, "-U") && value) {
            changes[change_count++] = snx_format("-%s", value);
            first++;
        } else if (!strncmp(option, "--fd=", 5) && adopted_count < 64 &&
                   sscanf(option + 5, "%d:%llu", &adopted_fd[adopted_count], &adopted_host[adopted_count]) == 2) {
            adopted_count++;
        } else if (!strcmp(option, "--cwd") && value) {
            cwd = value;
            first++;
        } else if (!strcmp(option, "-0") && value) {
            argv0 = value;
            first++;
        } else {
            fprintf(stderr, "scarletnx-gnuabi: unknown option %s (--help lists them)\n", option);
            return 2;
        }
        first++;
    }
    if (first == argc) {
        fputs(usage, stderr);
        return 2;
    }
    if (!isolated && !sysroot) sysroot = default_sysroot(argv[0]);
    if (!no_cache) {
        if (cache_option)
            cache = snx_absolute(cache_option);
        else {
            char* user = snx_user_dir(SNX_USER_CACHE);
            cache = user ? snx_path_join(user, "linux-jit") : NULL;
            free(user);
        }
    }
    process = calloc(1, sizeof *process);
    if (!process) return 2;
    char error[1024] = {0};
    if (gnu_jit_init(&process->jit, mode, cc, cache, error, sizeof error)) {
        fprintf(stderr, "scarletnx-gnuabi: %s\n", error);
        return 2;
    }
    process->jit.verbose = verbose;
    snx_recompiler_set_verbose(process->jit.recompiler, verbose);
    if (snx_recompiler_set_optimization(process->jit.recompiler, opt[0]) || opt[1]) {
        fprintf(stderr, "scarletnx-gnuabi: --jit-opt takes 0, 1, 2, 3, s or z\n");
        return 2;
    }
    char** env = isolated ? NULL : environment(changes, change_count);
    static const char* const isolated_environment[] = {"PATH=/bin", "LANG=C", NULL};
    const char** guest_argv = calloc((size_t)(argc - first) + 1, sizeof *guest_argv);
    if (!guest_argv) return 2;
    for (int i = first; i < argc; i++) guest_argv[i - first] = argv[i];
    if (argv0) guest_argv[0] = argv0;
    /* what the copies of this runtime a guest's execve starts get (Windows): this one's choices */
    {
        const char* forward[8];
        int n = 0;
        char *root_option = sysroot ? snx_format("--sysroot=%s", sysroot) : NULL, *cc_option = NULL,
             *cache_forward = NULL, *opt_option = snx_format("--jit-opt=%s", opt);
        forward[n++] = isolated ? "--isolated" : root_option ? root_option : "--isolated";
        forward[n++] = mode == GNU_AOT ? "--aot" : "--jit";
        if (cc) forward[n++] = cc_option = snx_format("--cc=%s", cc);
        forward[n++] = cache ? (cache_forward = snx_format("--jit-cache=%s", cache)) : "--no-jit-cache";
        forward[n++] = opt_option;
        gnu_spawn_set_options(n, forward);
        free(root_option);
        free(cc_option);
        free(cache_forward);
        free(opt_option);
    }
    if (gnu_process_init(process, argv[first], argc - first, guest_argv,
                         env ? (const char* const*)env : isolated_environment, isolated ? NULL : sysroot, error,
                         sizeof error)) {
        fprintf(stderr, "scarletnx-gnuabi: %s\n", error);
        return 2;
    }
    if (cwd && !isolated) gnu_vfs_set_cwd(&process->vfs, cwd);
    for (int i = 0; i < adopted_count; i++) {
#ifdef _WIN32
        int host = _open_osfhandle((intptr_t)adopted_host[i], 0);
#else
        int host = (int)adopted_host[i];
#endif
        if (host >= 0) gnu_vfs_adopt(&process->vfs, adopted_fd[i], host);
    }
#ifdef _WIN32
    HANDLE thread = CreateThread(NULL, GUEST_STACK, guest_thread, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (!thread) {
        fprintf(stderr, "scarletnx-gnuabi: cannot start the guest thread\n");
        return 2;
    }
    WaitForSingleObject(thread, INFINITE);
#else
    pthread_attr_t attributes;
    pthread_t thread;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, GUEST_STACK);
    if (pthread_create(&thread, &attributes, guest_thread, NULL)) {
        fprintf(stderr, "scarletnx-gnuabi: cannot start the guest thread\n");
        return 2;
    }
    pthread_join(thread, NULL);
#endif
    return run_status; /* not reached: the guest's thread ends the process */
}
