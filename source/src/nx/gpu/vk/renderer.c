/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Vulkan renderer: the renderer interface (vk_renderer, the vkr_ functions it points to) and presenting.

   Render targets, clears, draws (gpu/vk/render.c), compute (gpu/vk/compute.c), write-backs to and reloads from
   guest memory, and presenting the game's framebuffer from its render target (or from memory, converted on the CPU,
   when no render target holds it). Everything that records runs under vk_lock(), which is reentrant: submissions and
   presents on the GPU thread (gpu/gpu.c), the menu's refresh on the main thread (vkr_refresh), screenshots on the
   thread asking, and shader results landing on whichever thread reads their memory first. Acquiring a swapchain image
   never waits for the display: with every image still on it or queued for it, that frame isn't shown (counted). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nx/runtime/main.h"
#include "nx/runtime/options.h"
#include "nx/gpu/gpu.h"
#include "nx/hle/services/nvdrv.h"
#include "nx/runtime/platform/backend.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"
#include "nx/gpu/vk/vk.h"
#include "nx/gpu/renderer.h"

static VkSemaphore g_acquired[VK_FRAMES];
/* the CPU fallback's image, and the last presented frame for screenshots */
static Surf* g_cpu_img;
static Surf* g_last;
static u32 g_last_x, g_last_y, g_last_w, g_last_h;
static u32 g_last_transform;
static void vkr_before_write(u64 gva, u64 size) {
    if (size) vk_release_need(gva, size);
    vk_buf_before_write(gva, size);
    if (size)
        vk_gfx_image_mem_need(gva, size);
    else
        vk_gfx_image_land();
    vk_compute_before_write(gva, size);
    vk_rt_before_write(gva, size);
}
/* a barrier between GPU work: what a signal point does, but graphics storage images are copied out without waiting,
   to merge when done or read (every reader lands them first: gmem_need, the buffer fetches, the signal points) */
static void vkr_barrier(void) {
    vk_buf_before_write(0, 0);
    vk_gfx_image_copy();
    vk_compute_before_write(0, 0);
}
static void vkr_gfx_mem_need(u64 gva, u64 size) {
    vk_buf_mem_need(gva, size);
    vk_gfx_image_mem_need(gva, size);
}
static void vkr_mapping_begin(u64 gva, u64 size, int full) {
    vk_lock();
    vk_release_drain();
    if (full) {
        vk_buf_land_map();
        vk_gfx_image_land();
        vk_compute_land();
    } else {
        vk_buf_mem_need_map(gva, size);
        vk_gfx_image_mem_need(gva, size);
        vk_compute_mem_need_map(gva, size);
    }
}
static void vkr_mapping_end(void) { vk_unlock(); }

/* 0 when Vulkan can't run here */
static int vkr_start(void) {
    if (!vk_init()) return 0;
    g_vk_scale = g_opt_resf;       /* the .ini keeps resolution within 0.25 to 4 (options.c) */
    g_vk_anisotropy = g_opt_aniso; /* the sampler table keeps what it makes: anisotropy applies at start */
    g_vk_lod_bias = g_opt_lod_bias;
    if (g_vk_lod_bias != 0.0f) LOG("vulkan: texture LOD bias %+.2f on mipmapped 3D surfaces\n", (double)g_vk_lod_bias);
    if (g_vk_anisotropy > 1 && !g_vk.feat.samplerAnisotropy)
        LOG("vulkan: this GPU has no anisotropic filtering, so anisotropy=%d does nothing\n", g_vk_anisotropy);
    if (g_vk_scale != 1.0f) LOG("vulkan: render targets at %.2fx\n", g_vk_scale);
    vk_cmd_init();
    vk_pipeline_init();
    g_gpu_before_write = vkr_before_write;
    g_gpu_barrier = vkr_barrier;
    g_gpu_mapping_begin = vkr_mapping_begin;
    g_gpu_mapping_end = vkr_mapping_end;
    g_gpu_mem_changed = vk_mem_changed;
    VkSemaphoreCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    if (!g_opt_headless)
        for (int i = 0; i < VK_FRAMES; i++)
            if (!VK_OK(vkCreateSemaphore(g_vk.dev, &sci, NULL, &g_acquired[i]))) return 0;
    g_renderer_up = 1; /* a renderer is active */
    return 1;
}

/* ------------------------------------------------------------ the front end's calls */
/* a new epoch: the caches that trust what they checked in this one (buffers, constant buffers, shaders) check again */
static void vkr_mem_may_have_changed(void) { __atomic_add_fetch(&g_vk_epoch, 1, __ATOMIC_RELAXED); }
static void vkr_draw_texture(GpuChannel* ch) {
    vk_lock();
    vk_render_draw_texture(ch);
    vk_unlock();
}
static void vkr_clear(GpuChannel* ch, u32 v) {
    vk_lock();
    vk_render_clear(ch, v);
    vk_unlock();
}
static void vkr_draw(GpuChannel* ch, int indexed, u32 topology, u32 first, u32 count, u32 bv, u32 bi, u32 instances) {
    g_draws++;
    vk_lock();
    vk_render_draw(ch, indexed, topology, first, count, bv, bi, instances);
    vk_unlock();
}
static void vkr_samples_reset(GpuChannel* ch) {
    vk_lock();
    vk_query_reset(ch->id);
    vk_unlock();
}
static void vkr_samples_enable(GpuChannel* ch, int on) {
    vk_lock();
    vk_query_enable(ch->id, on);
    vk_unlock();
}
static int vkr_samples_report(GpuChannel* ch, u64 gva, u32 len, u64 ticks) {
    vk_lock();
    int r = vk_query_report(ch->id, gva, len, ticks);
    vk_unlock();
    return r;
}
/* Keep ordering at each method barrier. The earlier signal-profile timer measures the preceding CPU-visible
   landing, not this function. Direct barrier-cost profiling is recorded in diagnostics/barrier-attribution-20261028.md.
   In-pass barriers require framebuffer-local dependency semantics and cannot replace this global dependency blindly. */
static void vkr_method_barrier(int kind) {
    vk_lock();
    if (kind == 3) {
        /* The TIC/TSC words are read from guest memory on each bind. Expire cached texture backing as well. */
        vk_tex_invalidate_all();
        __atomic_add_fetch(&g_vk_epoch, 1, __ATOMIC_RELAXED);
    } else {
        vk_render_end();
        /* The GPU queue orders work across this barrier, including WAIT_FOR_IDLE. CPU-visible releases
           are gated on GPU completion by the release path instead of stalling this command stream. */
        VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                               .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                               .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
        VkDependencyInfo di = {
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
        vkCmdPipelineBarrier2(vk_cmd(), &di);
        if (kind == 2) {
            /* Texture invalidation orders later reads after earlier GPU writes. The release path handles
               CPU-visible results; waiting for every invalidation stalls games that issue them per draw. */
            vk_tex_invalidate_all();
            __atomic_add_fetch(&g_vk_epoch, 1, __ATOMIC_RELAXED);
        }
    }
    vk_unlock();
}
static int vkr_blit(const Surf2D* s, const Surf2D* d, int dx, int dy, int dw, int dh, double sx, double sy, double sdx,
                    double sdy) {
    vk_lock();
    int r = vk_blit2d(s, d, dx, dy, dw, dh, sx, sy, sdx, sdy);
    vk_unlock();
    return r; /* 0: the front end copies through memory, with the renderer's rt_flush and rt_reload around it */
}
static int vkr_vic_compose(const VicJob* j) {
    static int off = -1;
    if (off < 0) off = getenv("SNX_NO_VIC_GPU") != NULL;
    if (off) return 0;
    return vk_vic_compose(j); /* 0: the caller composes in guest memory. It takes vk_lock where it needs it */
}
static void vkr_rt_flush(u64 gva) {
    vk_lock();
    vk_gfx_image_mem_need(gva, 1);
    vk_rt_flush(gva);
    vk_unlock();
}
static void vkr_rt_reload(u64 gva) {
    vk_lock();
    vk_rt_reload(gva);
    vk_unlock();
}
static void vkr_dispatch(GpuChannel* ch, const u32* q, u64 code) {
    static int off = -1;
    if (off < 0) off = getenv("SNX_NO_COMPUTE") != NULL;
    if (off) return;
    r3d_pending_flush(); /* a merged draw the front end is holding comes first */
    vk_lock();
    vk_dispatch(ch, q, code);
    vk_unlock();
}
/* Pending shader results land before the stream signals or waits. */
static void vkr_before_syncwait(void) {
    vk_buf_land_syncwait();
    vk_gfx_image_land();
    vk_compute_land();
}
static void vkr_stream_incr(u32 id) { vk_release_incr(id); }

/* ------------------------------------------------------------ presenting */
/* the frame from memory when no render target holds it (the game drew it on the CPU, or a video frame) */
static Surf* cpu_frame(u64 cpu_addr, u32 w, u32 h, u32 pitch, u32 layout, u32 bhl, u32 fmt) {
    if (g_cpu_img && (g_cpu_img->w != w || g_cpu_img->h != h)) {
        vk_defer_image(g_cpu_img->img, g_cpu_img->mem, VK_NULL_HANDLE, g_cpu_img->att);
        if (g_last == g_cpu_img) g_last = NULL;
        free(g_cpu_img);
        g_cpu_img = NULL;
    }
    if (!g_cpu_img) {
        g_cpu_img = calloc(1, sizeof *g_cpu_img);
        if (!g_cpu_img) return NULL;
        g_cpu_img->w = g_cpu_img->sw = w;
        g_cpu_img->h = g_cpu_img->sh = h;
        g_cpu_img->f = vk_rt_format(0xD5); /* RGBA8 */
        g_cpu_img->layout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                .imageType = VK_IMAGE_TYPE_2D,
                                .format = VK_FORMAT_R8G8B8A8_UNORM,
                                .extent = {w, h, 1},
                                .mipLevels = 1,
                                .arrayLayers = 1,
                                .samples = VK_SAMPLE_COUNT_1_BIT,
                                .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
        VmaAllocationCreateInfo aci = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        if (!VK_OK(vmaCreateImage(g_vk.vma, &ii, &aci, &g_cpu_img->img, &g_cpu_img->mem, NULL))) {
            free(g_cpu_img);
            g_cpu_img = NULL;
            return NULL;
        }
    }
    VkBuffer buf;
    u64 off;
    u32* px = (u32*)vk_stage((u64)w * h * 4, 4, &buf, &off);
    if (!px) return NULL;
    vi_convert_cpu(cpu_addr, w, h, pitch, layout, bhl, fmt, px);
    VkCommandBuffer cb = vk_cmd();
    vk_surf_use(cb, g_cpu_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkBufferImageCopy c = {
        .bufferOffset = off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {w, h, 1}};
    vkCmdCopyBufferToImage(cb, buf, g_cpu_img->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    return g_cpu_img;
}

static int is_srgb(VkFormat f) {
    return f == VK_FORMAT_R8G8B8A8_SRGB || f == VK_FORMAT_B8G8R8A8_SRGB || f == VK_FORMAT_A8B8G8R8_SRGB_PACK32;
}

static int g_sc_srgb; /* what the swapchain was asked for (the surface may not have it) */
/* the window's surface: lost while the app is in the background (Android), made anew when it comes back */
static u32 g_generation;
static int g_lost;
static u64 g_shown_ns; /* when the game last gave the window a frame (atomic: vkr_refresh reads it unlocked) */
/* a blit's source flipped as the frame's Android transform says (FlipH 1, FlipV 2): the endpoints swapped */
static void flip_src(VkImageBlit* b, u32 transform) {
    if (transform & 1) {
        i32 x = b->srcOffsets[0].x;
        b->srcOffsets[0].x = b->srcOffsets[1].x;
        b->srcOffsets[1].x = x;
    }
    if (transform & 2) {
        i32 y = b->srcOffsets[0].y;
        b->srcOffsets[0].y = b->srcOffsets[1].y;
        b->srcOffsets[1].y = y;
    }
}

/* ---- the present thread: the game's frames reach the display on even slots ----
   show() submits a frame on the GPU thread and hands its present here: it goes out at its slot, the present before
   plus the game's frame period (present_period_ns), or at once when the frame comes later than that, so a late frame
   isn't followed by a short one. The game's own pace (present_pace) holds it at the frame's start, but the frame
   reaches the display at its present, after however much work: holding it here, on the GPU thread, would stall the
   fences the game waits on. A frame handed while the one before still waits sends that one out first. The slot comes
   PT_SLACK early, so the schedule drifts back toward the frames as they come and doesn't stay behind after a late
   one. Under vk_queue_lock, not vk_lock, which recording holds for long stretches: g_pt's fields and the numbers,
   and g_pt_lock with g_pt for the thread's wait */
#define PT_SLACK 250000ull /* ns */
typedef struct {
    VkSwapchainKHR sc; /* VK_NULL_HANDLE: none waiting */
    VkSemaphore sem;
    u32 idx;
    int game;
    u64 due, handed, seq;
} PtFrame;
static PtFrame g_pt;
static PMutex g_pt_lock = PMUTEX_INIT;
static PCond g_pt_cond;
static int g_pt_started;
static u64 g_pt_last;                        /* when the game's frame before was due */
static VkResult g_pt_result = VK_SUCCESS;    /* what show() acts on: out of date, suboptimal, surface lost */
static u32 g_pt_hist[512];                   /* game frames' times on the display, in 0.25 ms steps */
static u64 g_pt_shown, g_pt_prev, g_pt_held; /* game frames presented, the last one's time, their holds in ns */
static u32 g_pt_early, g_pt_lock_late;       /* sent out before their slot / over 1 ms past it */
static FILE* g_pt_times;                     /* SNX_PRESENT_TIMES: each game frame's time, hold and lateness, in us */

static void pt_report(void) {
    u32 i, n = 0, sum = 0, under = 0, over = 0;
    double median = 0, p99 = 0;
    for (i = 0; i < 512; i++) {
        n += g_pt_hist[i];
        if (i < 56) under += g_pt_hist[i];
        if (i >= 76) over += g_pt_hist[i];
    }
    for (i = 0; i < 512 && n; i++) {
        sum += g_pt_hist[i];
        if (median == 0 && sum * 2 >= n) median = (i + 1) * 0.25;
        if (p99 == 0 && (u64)sum * 100 >= (u64)n * 99) p99 = (i + 1) * 0.25;
    }
    if (n)
        LOG("vulkan: frames on the display: median %.2f ms, p99 %.2f ms; under 14 ms %u, over 19 ms %u of %u; held "
            "%.2f ms on average; %u sent before their slot, %u over 1 ms past it\n",
            median, p99, under, over, n, (double)g_pt_held / (double)g_pt_shown / 1e6, g_pt_early, g_pt_lock_late);
    memset(g_pt_hist, 0, sizeof g_pt_hist);
}

/* presents what waits. Under vk_queue_lock */
static void pt_present(void) {
    PtFrame f = g_pt;
    u64 now;
    pm_lock(&g_pt_lock);
    g_pt.sc = VK_NULL_HANDLE;
    pm_unlock(&g_pt_lock);
    if (!f.sc) return;
    VkPresentInfoKHR pi = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                           .waitSemaphoreCount = 1,
                           .pWaitSemaphores = &f.sem,
                           .swapchainCount = 1,
                           .pSwapchains = &f.sc,
                           .pImageIndices = &f.idx};
    VkResult r = vkQueuePresentKHR(g_vk.gfx, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR || r == VK_ERROR_SURFACE_LOST_KHR) {
        if (g_pt_result != VK_ERROR_SURFACE_LOST_KHR) g_pt_result = r;
    } else
        VK_OK(r);
    if (!f.game) return;
    now = plat_ns();
    g_pt_early += now < f.due;
    g_pt_lock_late += now > f.due + 1000000;
    if (g_pt_prev && __atomic_load_n(&g_pace_interval, __ATOMIC_RELAXED) <= 1) {
        u64 d = (now - g_pt_prev) / 250000;
        g_pt_hist[d < 511 ? d : 511]++;
        if (g_pt_times)
            fprintf(g_pt_times, "%llu %llu %lld\n", (unsigned long long)((now - g_pt_prev) / 1000),
                    (unsigned long long)((now - f.handed) / 1000), (long long)((i64)(now - f.due) / 1000));
    }
    g_pt_prev = now;
    g_pt_held += now - f.handed;
    if (++g_pt_shown % 600 == 0) pt_report();
}

static void pt_main(void* arg) {
    (void)arg;
    for (;;) {
        u64 seq, due, now;
        pm_lock(&g_pt_lock);
        while (!g_pt.sc) pc_wait(&g_pt_cond, &g_pt_lock, -1);
        seq = g_pt.seq;
        due = g_pt.due;
        pm_unlock(&g_pt_lock);
        now = plat_ns();
        if (due > now) plat_sleep_ns((i64)(due - now));
        vk_queue_lock();
        if (g_pt.sc && g_pt.seq == seq) pt_present(); /* not one show() sent out meanwhile */
        vk_queue_unlock();
    }
}

/* a swapchain going: what waits for it is dropped, never presented. Under vk_queue_lock */
void vk_present_drop(void) {
    pm_lock(&g_pt_lock);
    g_pt.sc = VK_NULL_HANDLE;
    pm_unlock(&g_pt_lock);
}

/* hands the frame just submitted to the present thread, or presents it at once when its slot has come (or it isn't
   one of the game's paced frames). Under vk_queue_lock */
static void pt_hand(u32 idx, VkSemaphore sem, int game) {
    static int off = -1; /* SNX_PRESENT_THREAD=0: each frame presented as it comes, as before, still measured */
    u64 now = plat_ns(), period = game ? present_period_ns() : 0, due = now;
    if (off < 0) {
        const char* t = getenv("SNX_PRESENT_TIMES");
        off = getenv("SNX_PRESENT_THREAD") && !strcmp(getenv("SNX_PRESENT_THREAD"), "0");
        if (t && *t) g_pt_times = fopen(t, "w");
    }
    if (period && !off && g_pt_last && g_pt_last + period - PT_SLACK > now) due = g_pt_last + period - PT_SLACK;
    if (game) g_pt_last = due;
    if (!g_pt_started && due > now) {
        pc_init(&g_pt_cond);
        g_pt_started = plat_thread(pt_main, NULL, 0) == 0 ? 1 : -1;
        if (g_pt_started < 0) WARN("vulkan: the present thread could not be started: frames go out as they come\n");
    }
    pm_lock(&g_pt_lock);
    g_pt.sc = g_vk.swapchain;
    g_pt.sem = sem;
    g_pt.idx = idx;
    g_pt.game = game;
    g_pt.due = due;
    g_pt.handed = now;
    g_pt.seq++;
    pm_unlock(&g_pt_lock);
    if (due <= now || g_pt_started < 0)
        pt_present();
    else
        pc_signal(&g_pt_cond);
}

/* The window's image: src's g_last_ rectangle (a frame of fw x fh, flipped by transform) letterboxed on black, or
   black alone, with the on-screen controls and the game's menu over it; then the frame's commands are submitted and
   the image presented. A game's frame (game) also moves the per-present bookkeeping, and without src shows nothing.
   Under vk_lock */
static void show(Surf* src, u32 fw, u32 fh, u32 transform, int game) {
    /* the size the swapchain was made for, not the extent the surface clamped it to: that can stay different */
    int ww, wh, srgb = src ? is_srgb(src->f.vk) : g_sc_srgb, shown = backend_window_shown();
    vk_queue_lock();
    if (g_pt.sc) pt_present(); /* the frame before still waits for its slot: it goes first, before any new swapchain */
    vk_queue_unlock();
    backend_window_size(&ww, &wh);
    if (shown && (g_lost || backend_window_generation() != g_generation)) {
        vk_finish();
        g_lost = !vk_surface_recreate();
        g_generation = backend_window_generation();
    }
    shown = shown && !g_lost;
    int tried = 0; /* a create already ran this frame: one that failed waits for the next frame */
    /* a new vsync in the .ini takes a new swapchain, as a new size does */
    if (shown && ww > 0 && wh > 0 &&
        (ww != g_vk.sc_win_w || wh != g_vk.sc_win_h || srgb != g_sc_srgb ||
         __atomic_load_n(&g_opt_vsync, __ATOMIC_RELAXED) != g_vk.sc_vsync)) {
        vk_finish();
        g_sc_srgb = srgb;
        vk_swapchain_create(srgb);
        tried = 1;
    }
    VkSemaphore wait = VK_NULL_HANDLE, signal = VK_NULL_HANDLE;
    u32 idx = 0;
    int recreate = 0;
    if (shown && (src || !game) && (g_vk.swapchain || (!tried && vk_swapchain_create(srgb)))) {
        VkCommandBuffer cb = vk_cmd(); /* waits for this frame slot, so its acquire semaphore is free */
        VkSemaphore acq = g_acquired[vk_frame_id() % VK_FRAMES];
        /* no wait: vk_lock is held (guest threads mapping memory take it too), and the game's pace is present_pace's.
           Every image still on the display or queued for it (FIFO behind) leaves this frame unshown */
        vk_queue_lock();
        VkResult r = vkAcquireNextImageKHR(g_vk.dev, g_vk.swapchain, 0, acq, VK_NULL_HANDLE, &idx);
        vk_queue_unlock();
        if (r == VK_NOT_READY || r == VK_TIMEOUT) {
            /* the game faster than the display (vsync=1 with fps_limit over its refresh, or a 59.94 Hz one) */
            static u64 dropped;
            if (game && ++dropped % 600 == 1)
                LOG("vulkan: %llu frames not shown, the display had every swapchain image\n",
                    (unsigned long long)dropped);
        } else if (r == VK_ERROR_OUT_OF_DATE_KHR)
            recreate = 1;
        else if (r == VK_ERROR_SURFACE_LOST_KHR)
            g_lost = 1;
        else if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) {
            wait = acq;
            signal = g_vk.sc_done[idx];
            VkImage sc = g_vk.sc_images[idx];
            if (src)
                vk_surf_use(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT,
                            VK_ACCESS_2_TRANSFER_READ_BIT);
            /* the submit waits for the acquire at the clear, the image's first use */
            vk_barrier(cb, sc, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_CLEAR_BIT, 0, VK_IMAGE_LAYOUT_UNDEFINED,
                       VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkClearColorValue black = {{0, 0, 0, 1}};
            VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdClearColorImage(cb, sc, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &all);
            vk_barrier(cb, sc, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT,
                       VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            /* letterbox: the largest rectangle of the frame's aspect that fits the window's part clear of the system's
               bars, centered there; the clear part scales with the extent the surface may have clamped */
            u32 sw = g_vk.sc_extent.width, sh = g_vk.sc_extent.height, cx = 0, cy = 0, cw = sw, ch = sh, dw, dh;
            int sx, sy, sww, swh;
            backend_window_safe(&sx, &sy, &sww, &swh);
            if (g_vk.sc_win_w > 0 && g_vk.sc_win_h > 0 && sww > 0 && swh > 0 && sx + sww <= g_vk.sc_win_w &&
                sy + swh <= g_vk.sc_win_h) {
                cx = (u32)((u64)sw * (u32)sx / (u32)g_vk.sc_win_w);
                cy = (u32)((u64)sh * (u32)sy / (u32)g_vk.sc_win_h);
                cw = (u32)((u64)sw * (u32)sww / (u32)g_vk.sc_win_w);
                ch = (u32)((u64)sh * (u32)swh / (u32)g_vk.sc_win_h);
            }
            if (src && fw && fh) {
                dw = cw;
                dh = (u32)((u64)cw * fh / fw);
                if (dh > ch) {
                    dh = ch;
                    dw = (u32)((u64)ch * fw / fh);
                }
                i32 dx = (i32)(cx + (cw - dw) / 2), dy = (i32)(cy + (ch - dh) / 2);
                VkImageBlit blit = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                    .srcOffsets = {{(i32)g_last_x, (i32)g_last_y, 0},
                                                   {(i32)(g_last_x + g_last_w), (i32)(g_last_y + g_last_h), 1}},
                                    .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                    .dstOffsets = {{dx, dy, 0}, {dx + (i32)dw, dy + (i32)dh, 1}}};
                flip_src(&blit, transform); /* render targets stay in guest order */
                vkCmdBlitImage(cb, src->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sc,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
            }
            {
                int safe[4] = {(int)cx, (int)cy, (int)cw, (int)ch};
                vk_ui_draw(cb, sc, safe);
            }
            vk_barrier(cb, sc, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0,
                       VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
        } else
            VK_OK(r);
    }
    if (game) {
        g_vk_present++;
        if (g_last) g_last->last_used = g_vk_present; /* what screenshots read stays */
        vk_tex_frame();
        vk_buf_frame();
        if (g_vk_present % 600 == 0) {
            LOG("vulkan: %llu draws, %llu skipped; %llu dispatches in %llu batches (%llu ms waiting), %llu skipped; "
                "%llu finishes (%llu ms)\n",
                (unsigned long long)g_vk_drawn, (unsigned long long)g_vk_skipped, (unsigned long long)g_vk_dispatches,
                (unsigned long long)g_vk_cs_batches,
                (unsigned long long)(__atomic_load_n(&g_vk_cs_wait_ns, __ATOMIC_RELAXED) / 1000000),
                (unsigned long long)g_vk_cs_skipped, (unsigned long long)g_vk_finishes,
                (unsigned long long)(g_vk_finish_ns / 1000000));
        }
    }
    /* Offscreen frames still submit and advance the frame ring. g_last remains available for readback. */
    vk_submit(wait, VK_PIPELINE_STAGE_2_CLEAR_BIT, signal);
    vk_frame_next(); /* every show used this frame's slot and acquire semaphore, the menu's own too */
    vk_queue_lock();
    if (signal) pt_hand(idx, signal, game);
    /* what the presents since the last show() answered, this one's when it went out at once */
    if (g_pt_result == VK_ERROR_OUT_OF_DATE_KHR || (g_pt_result == VK_SUBOPTIMAL_KHR && !g_vk.sc_turned))
        recreate = 1;
    else if (g_pt_result == VK_ERROR_SURFACE_LOST_KHR)
        g_lost = 1;
    g_pt_result = VK_SUCCESS;
    vk_queue_unlock();
    if (recreate && !g_lost && !g_vk.sc_lost) { /* not when this frame's create found the surface lost */
        vk_finish();
        vk_swapchain_create(srgb);
    }
    if (g_vk.sc_lost) { /* a create found the surface lost: the next frame makes it anew */
        g_vk.sc_lost = 0;
        g_lost = 1;
    }
}

/* the NVN path's replay (nvn_res.c, phase 3 step 3c part 3): the next present of the buffer at g_capture_cpu is
   saved as g_capture_path.ppm, the renderer's own picture of the frame the replay drew */
static u64 g_capture_cpu;
static char g_capture_path[600];
void vk_capture_next_present(u64 cpu_addr, const char* path) {
    vk_lock();
    g_capture_cpu = cpu_addr;
    snprintf(g_capture_path, sizeof g_capture_path, "%s", path);
    vk_unlock();
}
static u64 surf_save(Surf* s, u32 x, u32 y, u32 w, u32 h, u32 transform, const char* path);

/* the game queued a buffer for display: show it, letterboxed to the window. A frame that can't be shown (minimized,
   no image to draw to) is still submitted, with the per-present bookkeeping, so frames don't pile up */
static void vkr_present_buffer(u64 cpu_addr, u32 w, u32 h, u32 pitch, u32 layout, u32 bhl, u32 fmt, const i32 crop[4],
                               u32 transform) {
    vk_lock();
    vk_buf_writable_abort(); /* a draw abandoned during setup has no shader writes to land */
    vk_render_end();
    Surf* src = NULL;
    if (w && h && w <= g_vk.props.limits.maxImageDimension2D && h <= g_vk.props.limits.maxImageDimension2D) {
        src = vk_nvn_live_surf(cpu_addr);
        if (!src) src = vk_rt_for_cpu(cpu_addr, w, h);
        if (!src || src->f.is_int) src = cpu_frame(cpu_addr, w, h, pitch, layout, bhl, fmt);
    }
    /* the part of the buffer shown: vi's crop, or all of it */
    u32 x0 = 0, y0 = 0, x1 = w, y1 = h;
    if (crop[2] > crop[0] && crop[3] > crop[1] && crop[0] >= 0 && crop[1] >= 0) {
        x0 = (u32)crop[0] < w ? (u32)crop[0] : 0;
        y0 = (u32)crop[1] < h ? (u32)crop[1] : 0;
        x1 = (u32)crop[2] < w ? (u32)crop[2] : w;
        y1 = (u32)crop[3] < h ? (u32)crop[3] : h;
    }
    if (src) { /* the render target found for the buffer can be smaller than it: a crop outside it shows all of it */
        if (x1 > src->w) x1 = src->w;
        if (y1 > src->h) y1 = src->h;
        if (x1 <= x0 || y1 <= y0) {
            x0 = y0 = 0;
            x1 = w < src->w ? w : src->w;
            y1 = h < src->h ? h : src->h;
        }
    }
    if (src) { /* in its pixels: targets are scaled */
        g_last = src;
        g_last_transform = transform;
        g_last_x = (u32)((u64)src->sw * x0 / src->w);
        g_last_y = (u32)((u64)src->sh * y0 / src->h);
        g_last_w = (u32)((u64)src->sw * x1 / src->w) - g_last_x;
        g_last_h = (u32)((u64)src->sh * y1 / src->h) - g_last_y;
    }
    if (g_capture_cpu && g_capture_cpu != ~0ull && cpu_addr != g_capture_cpu) {
        static u32 seen;
        if (seen++ < 6)
            LOG("vulkan: a capture waits for buffer %llx; presented %llx (%ux%u)\n", (unsigned long long)g_capture_cpu,
                (unsigned long long)cpu_addr, w, h);
    }
    if (g_capture_cpu && (cpu_addr == g_capture_cpu || g_capture_cpu == ~0ull)) { /* ~0: the next present */
        g_capture_cpu = 0;
        if (src)
            LOG("vulkan: the presented buffer %llx saved as %s.ppm (%ux%u of a %ux%u target, transform %u), hash "
                "%016llx\n",
                (unsigned long long)cpu_addr, g_capture_path, g_last_w, g_last_h, src->sw, src->sh, transform,
                (unsigned long long)surf_save(src, g_last_x, g_last_y, g_last_w, g_last_h, transform, g_capture_path));
    }
    show(src, x1 - x0, y1 - y0, transform, 1);
    __atomic_store_n(&g_shown_ns, plat_ns(), __ATOMIC_RELAXED);
    vk_unlock();
}

/* The last frame again, under the controls and the menu, while the game shows none: a game loading, stuck, or drawing
   nothing still has them. Not before a quarter second without a frame (a slow game's own come between), at 30 a second
   at most, and only when the GPU thread holds no Vulkan work: then the next call tries again */
static void vkr_refresh(void) {
    static u64 last;
    u64 now = plat_ns();
    if (!backend_window_shown() || now - __atomic_load_n(&g_shown_ns, __ATOMIC_RELAXED) < 250000000ull ||
        now - last < 33000000ull)
        return;
    if (!vk_trylock()) return;
    last = now;
    vk_render_end();
    show(g_last, g_last_w, g_last_h, g_last_transform, 0);
    vk_unlock();
}

/* a part of a surface, blitted to RGBA8 and read back: its hash, and a .ppm at path if given */
static u64 surf_save(Surf* s, u32 x, u32 y, u32 w, u32 h, u32 transform, const char* path) {
    u64 hash = 1469598103934665603ull;
    /* blit to an RGBA8 image, whatever the target's format, then read that back. sRGB to sRGB, so the bytes stay
       encoded as the display shows them */
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                            .imageType = VK_IMAGE_TYPE_2D,
                            .format = is_srgb(s->f.vk) ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM,
                            .extent = {w, h, 1},
                            .mipLevels = 1,
                            .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                            .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VmaAllocationCreateInfo aci = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VkImage tmp;
    VmaAllocation tmem;
    if (!VK_OK(vmaCreateImage(g_vk.vma, &ii, &aci, &tmp, &tmem, NULL))) return hash;
    VkBuffer buf;
    u8* p = vk_readback_buffer((u64)w * h * 4, &buf);
    if (!p) {
        vmaDestroyImage(g_vk.vma, tmp, tmem);
        return hash;
    }
    vk_render_end(); /* the blit, the copy and their barriers can't be recorded inside a rendering pass */
    VkCommandBuffer cb = vk_cmd();
    vk_surf_use(cb, s, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT);
    vk_barrier(cb, tmp, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT, 0, VK_IMAGE_LAYOUT_UNDEFINED,
               VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageBlit blit = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                        .srcOffsets = {{(i32)x, (i32)y, 0}, {(i32)(x + w), (i32)(y + h), 1}},
                        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                        .dstOffsets = {{0, 0, 0}, {(i32)w, (i32)h, 1}}};
    flip_src(&blit, transform); /* screenshots show the window's transform, offscreen and headless presents too */
    vkCmdBlitImage(cb, s->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, tmp, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                   &blit, VK_FILTER_NEAREST);
    vk_barrier(cb, tmp, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy c = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {w, h, 1}};
    vkCmdCopyImageToBuffer(cb, tmp, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &c);
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                           .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                           .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                           .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
                           .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &di);
    vk_finish();
    vk_readback_ready(0, (u64)w * h * 4);
    vmaDestroyImage(g_vk.vma, tmp, tmem);
    const u32* px = (const u32*)p;
    for (u64 i = 0; i < (u64)w * h; i++) hash = (hash ^ px[i]) * 1099511628211ull;
    if (path) {
        char fn[1100];
        snprintf(fn, sizeof fn, "%s.ppm", path);
        FILE* f = fopen(fn, "wb");
        int ok = f != NULL;
        if (f) {
            fprintf(f, "P6\n%u %u\n255\n", w, h);
            for (u64 i = 0; i < (u64)w * h && ok; i++) {
                u8 rgb[3] = {(u8)px[i], (u8)(px[i] >> 8), (u8)(px[i] >> 16)};
                ok = fwrite(rgb, 1, 3, f) == 3;
            }
            ok = fclose(f) == 0 && ok;
        }
        if (!ok) WARN_LIMIT(1, "vulkan: the screenshot %s couldn't be written\n", fn);
    }
    return hash;
}
/* the last presented frame as PPM (path.ppm), and a hash of its pixels */
static u64 vkr_screenshot(const char* path) {
    vk_lock();
    u64 hash = g_last ? surf_save(g_last, g_last_x, g_last_y, g_last_w, g_last_h, g_last_transform, path)
                      : 1469598103934665603ull;
    vk_unlock();
    return hash;
}

const Renderer vk_renderer = {
    .name = "Vulkan",
    .start = vkr_start,
    .mem_may_have_changed = vkr_mem_may_have_changed,
    .clear = vkr_clear,
    .draw_texture = vkr_draw_texture,
    .draw = vkr_draw,
    .method_barrier = vkr_method_barrier,
    .blit = vkr_blit,
    .vic_compose = vkr_vic_compose,
    .rt_flush = vkr_rt_flush,
    .rt_reload = vkr_rt_reload,
    .dispatch = vkr_dispatch,
    .cs_mem_need = vk_compute_mem_need,
    .gfx_mem_need = vkr_gfx_mem_need,
    .before_syncwait = vkr_before_syncwait,
    .stream_incr = vkr_stream_incr,
    .release_write = vk_release_write,
    .release_need = vk_release_need,
    .release_value = vk_release_value,
    .release_retire = vk_release_retire,
    .samples_reset = vkr_samples_reset,
    .samples_enable = vkr_samples_enable,
    .samples_report = vkr_samples_report,
    .stream_end = vk_release_submit,
    .present_buffer = vkr_present_buffer,
    .screenshot = vkr_screenshot,
    .refresh = vkr_refresh,
    .save = vk_pipeline_save,
    .release_window = vk_window_release,
};
const Renderer* g_renderer = &vk_renderer;
