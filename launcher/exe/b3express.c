/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the B3-Express contributors */
/* B3-Express.exe: starts the launcher window, launcher\python\pythonw.exe -E -s launcher\web_launcher.py, from the
   folder this program is in, and ends. No console. A package missing its Python says so instead of doing nothing. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wchar.h>

static void say(const wchar_t* text) { MessageBoxW(NULL, text, L"B3-Express", MB_OK | MB_ICONERROR); }

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR args, int show) {
    wchar_t dir[MAX_PATH], python[MAX_PATH + 64], script[MAX_PATH + 64], cmd[3 * MAX_PATH + 64];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    DWORD n = GetModuleFileNameW(NULL, dir, MAX_PATH);
    wchar_t* slash;
    (void)inst;
    (void)prev;
    (void)args;
    (void)show;
    ZeroMemory(&si, sizeof si);
    si.cb = sizeof si;
    if (!n || n >= MAX_PATH || !(slash = wcsrchr(dir, L'\\'))) {
        say(L"B3-Express couldn't find its own folder. Move the package to a shorter path and try again.");
        return 1;
    }
    *slash = 0;
    swprintf(python, sizeof python / sizeof *python, L"%ls\\launcher\\python\\pythonw.exe", dir);
    swprintf(script, sizeof script / sizeof *script, L"%ls\\launcher\\web_launcher.py", dir);
    if (GetFileAttributesW(python) == INVALID_FILE_ATTRIBUTES || GetFileAttributesW(script) == INVALID_FILE_ATTRIBUTES) {
        say(L"The launcher's files are missing (launcher\\python or launcher\\web_launcher.py).\n\n"
            L"Unzip the whole package again, keeping its folders, and start B3-Express.exe from there.");
        return 1;
    }
    swprintf(cmd, sizeof cmd / sizeof *cmd, L"\"%ls\" -E -s \"%ls\"", python, script);
    if (!CreateProcessW(python, cmd, NULL, NULL, FALSE, 0, NULL, dir, &si, &pi)) {
        say(L"The launcher couldn't start. Unzip the whole package again, or run it from a folder you can write to.");
        return 1;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}
