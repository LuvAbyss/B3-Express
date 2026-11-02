/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Forced include only in the production tree translation unit. */
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
HANDLE WINAPI fs_test_FindFirstFileA(LPCSTR path, LPWIN32_FIND_DATAA data);
HANDLE WINAPI fs_test_FindFirstFileW(LPCWSTR path, LPWIN32_FIND_DATAW data);
#define FindFirstFileA fs_test_FindFirstFileA
#define FindFirstFileW fs_test_FindFirstFileW
#else
#include <dirent.h>
DIR* fs_test_opendir(const char* path);
#define opendir fs_test_opendir
#endif
