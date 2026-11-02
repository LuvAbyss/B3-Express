/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The window backend's side of the runtime (backend/sdl3.c, windowed or headless): window, input, presenting, and
   the window half of the Vulkan renderer. */
#ifndef SNX_BACKEND_H
#define SNX_BACKEND_H
#include "common/cpu/aarch64/cpu.h"
#include "nx/runtime/ini.h"

void video_init(const char* title);
void video_poll(void);     /* events and input, once per main loop pass */
void frontend_frame(void); /* screenshots and the unattended limits, after each pass */

/* the backend's .ini settings (the touch screen's mode) */
extern const IniOpt g_backend_ini[];
/* Offers a .ini or command line setting to the backend, the on-screen controls and the unattended options: 1 when
   one of them took it */
int backend_ini(const char* k, const char* v);
/* the touch screen's mode chosen in the game's menu, saved to the .ini: 0 off, 1 fingers, 2 fingers and the mouse */
void backend_touch_set(int mode);

/* The window in pixels, for the swapchain; 0 x 0 when headless */
void backend_window_size(int* w, int* h);
/* The part of the window not under system bars or a camera cutout, in pixels (the whole window when fullscreen) */
void backend_window_safe(int* x, int* y, int* w, int* h);
/* 0 while nothing presented can be seen: the window minimized or covered, or its surface taken away (Android in the
   background) */
int backend_window_shown(void);
/* changes whenever the surface comes back, so the renderer knows to make a new Vulkan surface */
u32 backend_window_generation(void);

/* fatal's message box. Called on every platform and from any thread; does nothing where there is no box */
void win_fatal_box(const char* msg);

/* Vulkan, unused when headless. The two handle types are declared here as vulkan_core.h declares them for 64-bit
   hosts (a repeated identical typedef is valid), so callers need no Vulkan header */
typedef struct VkInstance_T* VkInstance;
typedef struct VkSurfaceKHR_T* VkSurfaceKHR;
void* backend_vk_loader(void); /* the address of vkGetInstanceProcAddr */
const char* const* backend_vk_extensions(u32* n); /* the instance extensions the window needs */
int backend_vk_surface(VkInstance instance, VkSurfaceKHR* surface);

#endif
