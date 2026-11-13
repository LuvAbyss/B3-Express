/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Vulkan renderer: shared state and the interfaces between its parts. Functions are loaded through volk from the
   loader the window backend gives (SDL's), or from libvulkan itself in headless runs (vk_init), so nothing links
   against a Vulkan library.

   device.c    instance, device, queues, swapchain, memory allocator (VMA)
   cmd.c       frames in flight: the command buffer being recorded, staging and readback rings, deferred destruction
   format.c    guest render target and texture formats -> Vulkan formats
   surface.c   render targets by guest address (a table textures share), loads from and write-backs to guest memory
   writeback.c landing compute and graphics storage results in guest memory
   texture.c   textures from the guest's texture headers (TIC), views and samplers (TSC)
   astc.c      compute decoding of ASTC texture blocks during upload
   buffer.c    vertex, index, constant and storage buffers from guest memory
   pipeline.c  translated shaders, the pipeline layout and pipelines by state
   render.c    the render target set (dynamic rendering), clears and draws
   compute.c   compute dispatches (on a queue of their own when there is one), landing their results in guest memory
   renderer.c  the renderer interface the GPU front end calls (gpu/renderer.h) and presenting
   ui.c        the on-screen controls and the game's menu, drawn over the swapchain image
   vma.cpp     VulkanMemoryAllocator's implementation

   Everything that records commands runs under vk_lock(), which is reentrant: submits and presents come from the GPU
   thread (gpu/gpu.c, hle/services/vi.c's present_queued), the menu's refresh from the main thread, and compute results
   land on whichever thread reads their memory first. */
#ifndef SNX_VK_H
#define SNX_VK_H
#include "common/cpu/aarch64/cpu.h"
#include "nx/gpu/gpu.h"
#include "volk.h"
#include "vk_mem_alloc.h"
#include "nx/gpu/shader/shader.h"

#define VK_FRAMES 2  /* frames in flight */
#define VK_SC_MAX 16 /* swapchain images */

typedef struct {
    VkInstance instance;
    VkDebugUtilsMessengerEXT debug;
    VkSurfaceKHR surface;
    VkPhysicalDevice phys;
    VkPhysicalDeviceProperties props;
    VkDevice dev;
    VmaAllocator vma;
    /* compute is a second queue of the graphics family, or of a compute-only family, when the device has one; else
       it is the graphics queue itself */
    u32 gfx_family, compute_family;
    VkQueue gfx, compute;
    /* swapchain */
    VkSwapchainKHR swapchain;
    VkExtent2D sc_extent;
    int sc_win_w, sc_win_h; /* the window size it was made for (sc_extent: that as the surface takes it) */
    int sc_srgb;            /* its format is sRGB: what presented frames in sRGB formats are blitted to */
    VkFormat sc_format;     /* its images' format, which the on-screen controls draw to (gpu/vk/ui.c) */
    int sc_vsync; /* the vsync setting it was made for (2: FIFO; the others are FIFO too on a surface with nothing else) */
    int sc_turned; /* upright on a display held another way: Android calls each present suboptimal for it */
    int sc_lost;   /* the last create found the surface lost: show() has it made anew */
    u32 sc_count;
    VkImage sc_images[VK_SC_MAX];
    VkSemaphore sc_done[VK_SC_MAX]; /* per swapchain image: rendering into it finished, it can be presented */
    /* optional features that were enabled */
    VkPhysicalDeviceFeatures feat;
    int has_divisor;      /* VK_EXT_vertex_attribute_divisor: instance divisors above 1 */
    int has_border_color; /* VK_EXT_custom_border_color */
    u32 border_color_max; /* maxCustomBorderColorSamplers: past it, samplers take the nearest built-in color */
    int has_mirror_clamp; /* samplerMirrorClampToEdge: VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE */
    int has_shader_draw_parameters;
    /* VK_EXT_external_memory_host: guest memory imported as Vulkan memory (nvn_res.c), its pointers and sizes aligned
       to host_import_align */
    int has_host_import;
    u64 host_import_align;
    int has_indirect_count; /* drawIndirectCount (Vulkan 1.2), for the NVN path's MultiDrawElementsIndirectCount */
    int has_sparse;         /* sparse buffers with holes, bound on the graphics queue (the NVN path's pool copies) */
    int has_gpl;            /* graphics pipeline libraries with fast linking: pipeline.c builds pipelines from them */
} VkState;
extern VkState g_vk;

/* device.c */
int vk_init(void);
/* (re)creates the swapchain at the window's size, in an sRGB format when srgb is set and the surface has one, with
   the present mode the vsync setting asks for, recorded in sc_vsync (show() makes a new one when the setting moves).
   0 while the window has no area (minimized) or its surface can't be queried, the old swapchain staying, or when the
   create failed: then there is none. A lost surface sets sc_lost; the next present tries again */
int vk_swapchain_create(int srgb);
int vk_surface_recreate(void);
int vk_window_release(void); /* Renderer.release_window */
void vk_present_drop(void);  /* renderer.c: a frame waiting to be presented on a swapchain that goes is dropped */
/* logs a failed call with where it happened; evaluates to 1 on success */
#define VK_OK(call) vk_ok_((call), #call, __FILE__, __LINE__)
int vk_ok_(VkResult r, const char* what, const char* file, int line);
void vk_lock(void);
void vk_unlock(void);
int vk_trylock(void); /* vk_lock unless another thread holds it: 1 when taken */
/* the queues and the swapchain alone, reentrant: every submit, acquire, present, device wait and swapchain change
   holds it, so the present thread presents without waiting for vk_lock (which recording holds). Taken after vk_lock,
   never before it */
void vk_queue_lock(void);
void vk_queue_unlock(void);
/* What an allocation is for, counted for the log: tagged when made, untagged before it's destroyed. Allocations never
   tagged show as the rest of what VMA holds */
enum { VKM_NONE, VKM_TARGET, VKM_SURF, VKM_ASTC, VKM_BC, VKM_TEX, VKM_COPY, VKM_STAGE, VKM_BUF, VKM_KINDS };
void vk_mem_tag(VmaAllocation a, int kind);
void vk_mem_untag(VmaAllocation a);
u64 vk_mem_budget(void); /* the video memory the process may use now, as the driver reports it */
void vk_mem_log(void); /* one line: video memory by kind, and the process's use against its budget */

/* cmd.c */
void vk_cmd_init(void);
VkCommandBuffer vk_cmd(void); /* the current frame's command buffer, begun on first use */
/* size bytes of the current frame's staging memory (host visible, flushed when the frame is submitted), valid until
   that frame's work is done */
u8* vk_stage(u64 size, u64 align, VkBuffer* buf, u64* off);
/* As vk_stage, with the allocation for GPU-written data that must be invalidated after its frame fence. */
u8* vk_stage_output(u64 size, u64 align, VkBuffer* buf, u64* off, VmaAllocation* mem);
/* Host-cached memory for the current frame's GPU -> host copies, read after its fence once invalidated. */
u8* vk_stage_back(u64 size, VkBuffer* buf, u64* off, VmaAllocation* mem);
/* ends and submits the commands recorded since the last submit, waiting for wait (at wait_stage) and signaling signal.
   The frame goes on: its staging, pools and deferred resources live until vk_frame_next and its slot's next use.
   Either semaphore may be VK_NULL_HANDLE */
void vk_submit(VkSemaphore wait, VkPipelineStageFlags2 wait_stage, VkSemaphore signal);
void vk_finish(void); /* submits and waits until the GPU has done everything, and lands every result */
extern u64 g_vk_finishes, g_vk_finish_ns; /* statistics: vk_finish calls and their wait (the GPU thread idles) */
void vk_frame_next(void);                 /* ends the frame (a present): the next records in the other slot */
void vk_frame_check(void);                /* at the start of a draw or dispatch: ends a frame that has grown too big */
/* a buffer for GPU -> host copies: record the copy into it, vk_finish(), vk_readback_ready(), then read */
u8* vk_readback_buffer(u64 size, VkBuffer* buf);
/* the bytes [off, off + size) written by the copies recorded into it since the last readback */
void vk_readback_ready(u64 off, u64 size);
/* invalidates the GPU's writes over [from, to) of a host-visible allocation for the host */
VkResult vk_invalidate_range(VmaAllocation mem, u64 from, u64 to);
/* destroyed once the frame being recorded now is done (the GPU may still use them until then) */
void vk_defer_image(VkImage img, VmaAllocation a, VkImageView v1, VkImageView v2);
void vk_defer_buffer(VkBuffer b, VmaAllocation a);
void vk_defer_buffer_view(VkBufferView v);
/* the submission being recorded (a number that grows at every submit: what a submit may have changed, results landed,
   is checked against it), and the frame (grows at vk_frame_next: frame f's work is done once f + VK_FRAMES started) */
u64 vk_submit_number(void);
u64 vk_frame_id(void);
u64 vk_last_submit(void);        /* the last submission's number, which its completion signals */
int vk_submit_done(u64 value);   /* the GPU has finished submission value */
void vk_wait_submit(u64 value);  /* until the GPU has done that submission (any thread, without vk_lock) */
void vk_land_through(u64 value); /* the results of the submissions up to value, done, into guest memory (vk_lock) */
void vk_barrier(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect, VkPipelineStageFlags2 src_stage,
                VkAccessFlags2 src_access, VkImageLayout from, VkPipelineStageFlags2 dst_stage,
                VkAccessFlags2 dst_access, VkImageLayout to);
/* a descriptor set from the current frame's pools, freed when the frame is reused */
VkDescriptorSet vk_desc_alloc(VkDescriptorSetLayout layout);
/* a descriptor pool's sizes (VK_POOL_TYPES entries) for `sets` sets of the stage layouts (gpu/vk/pipeline.c) */
#define VK_POOL_TYPES 5 /* descriptor types in a set: uniform, storage buffers, textures, images, texel buffers */
void vk_pool_sizes(VkDescriptorPoolSize* sz, u32 sets);
extern u32 g_vk_buffer_image_slots; /* storage texel buffer bindings in the compute set, from SNX_VKB_IMGBUF */
/* the staging alignment that suits every use: uniform, storage, vertex and index buffers */
u64 vk_stage_align(void);
/* the slack a vk_stage_back reservation needs: a ranged invalidation aligns its end up to an atom (cmd.c) */
u64 vk_stage_pad(void);
u64 vk_hash(const void* p, u64 size);
/* a hash of guest memory: all of it when small, else its ends and 62 samples between them */
u64 vk_sample_hash(u64 gva, u64 len);
/* guest submits processed so far, and frames presented: what the caches check memory against */
extern u32 g_vk_epoch, g_vk_present;

/* format.c */
typedef struct {
    VkFormat vk;
    u8 bpp; /* bytes per pixel (per block for compressed formats) */
    u8 bw, bh;
    u8 depth, stencil, is_int;
    u8 raw;  /* guest memory holds exactly the Vulkan format's bytes: it can be copied both ways */
    u8 sint; /* signed integer (is_int) */
    u8 conv; /* textures: converted on upload (VKC_*), the Vulkan format is what it converts to */
    u8 bgra; /* stored B, G, R, A in memory: a texture header's swizzle swaps R and B */
} VkFmt;
enum {
    VKC_NONE,
    VKC_ASTC,
    VKC_RGBA4,
    VKC_RGB5A1,
    VKC_RGB32F,
    VKC_A5B5G5R1,
    VKC_D24S8,
    VKC_S8D24,
    VKC_D24X8,
    VKC_D32S8,
    /* ETC2 and EAC, decoded on the CPU (gpu/texture/etc2.c): in the order of its kinds */
    VKC_ETC2,
    VKC_ETC2_PTA,
    VKC_ETC2_RGBA,
    VKC_EAC_R,
    VKC_EAC_R_SNORM,
    VKC_EAC_RG,
    VKC_EAC_RG_SNORM
};
VkFmt vk_rt_format(u32 fmt);
VkFmt vk_zt_format(u32 fmt);
VkFmt vk_tic_format(u32 w0, int srgb); /* bpp 0: not supported */
void vk_depth_unpack(const VkFmt* f, const u8* src, u64 pixels, u8* depth, u8* stencil);
int vk_tic_reads_stencil(const u32* tic);
/* sRGB and linear views of the same bytes: the twin of a color format, or the format itself when it has none */
VkFormat vk_srgb_as(VkFormat f, int srgb);
typedef struct {
    VkImageFormatListCreateInfo list;
    VkFormat f[2];
} VkViewFormats;
/* an image of a color format that has an sRGB twin is viewed as both: sets MUTABLE_FORMAT and chains the list of
   the two to ii (vf has to live until the image is created) */
void vk_view_formats(VkImageCreateInfo* ii, VkViewFormats* vf);

/* gpu/vk/surface.c: a render target or a texture */
typedef struct {
    u64 key; /* view type | swizzle << 4 | format << 20 | stencil aspect << 60 (vk_tex_view) */
    VkImageView v;
} SurfView;
typedef struct Surf {
    u64 gva;
    u32 w, h, layers, levels;
    u32 sw, sh;  /* the image's size: render targets are w x h times the resolution scale */
    u32 depth3d; /* 3D textures: depth (layers is 1) */
    u32 fkey; /* render targets: guest format | 0x10000 for depth. Textures: TIC word 0's format bits (0x7FFFF), sRGB
                    in bit 31, raw (vk_tex_from_tic_raw) in bit 30 */
    VkFmt f;
    u32 bl, bh, bd, pitch; /* guest layout: block linear with block height 2^bh GOBs, or pitch linear */
    u32 tws;               /* block linear: the header's tile width spacing (texture.c bl_row_bytes) */
    u64 layer_size;        /* bytes from one layer to the next in guest memory */
    int is_rt;
    int storage_capable;  /* image was created with VK_IMAGE_USAGE_STORAGE_BIT */
    int volume;           /* render target layers use 3D block-linear memory rather than array strides */
    VkImageViewType type; /* what it was created as */
    VkImage img;
    VmaAllocation mem;
    VkImageView att;        /* level 0: the attachment */
    VkImageView att_linear; /* same storage without sRGB conversion */
    VkImageLayout layout;   /* the whole image is kept in one layout */
    VkPipelineStageFlags2 stage;
    VkAccessFlags2 access;
    u64 last_used;
    u32 wgen;       /* render targets: bumped when drawn to */
    u32 drawn_at;   /* render targets: g_vk_present when last drawn to */
    u64 draw_gw;    /* guest-memory generation at the first draw this present or since a full replacement */
    u64 draw_hash;  /* fallback guest-memory hash at that baseline */
    u32 draw_wseen; /* CPU write-tracking clock at that baseline, zero after a reload */
    int draw_has_hash;
    int feeds_tex; /* a texture asked for its pixels (vk_rt_drawn_fresh): without write tracking, its baselines are hashed */
    /* render targets: the two textures last copied from it (gpu/vk/texture.c), with its wgen and the texture's upload
       generation then, so two textures over one target don't copy it again on each other's binds */
    struct {
        struct Surf* t;
        u32 wgen, tgen;
    } copied[2];
    u32 mem_wgen; /* render targets: wgen when guest memory last held the same (written back or loaded) */
    u64 gw;       /* gpu_mem_generation() when it last matched guest memory or its unchanged range was checked */
    /* textures: views by type and swizzle, and the change check (gpu/vk/texture.c) */
    SurfView sv[6];
    int nsv;
    u64 size, hash, shash;
    int upload_valid; /* textures: every subresource was uploaded (a failed upload is retried) */
    int uploaded;     /* textures: an upload has completed once: its image holds something to show */
    u32 checked, next_check, stable, tfull;
    u32 tex_inval_gen; /* explicit Maxwell texture-cache invalidations since this texture was checked */
    u32 wseen;         /* write-tracking clock when it last matched memory */
    /* copies of regions of this render target for texture headers or feedback draws, one per size, linked through next
       (gpu/vk/texture.c rt_crop) */
    struct Surf* crop;
    u32 crop_gen;
    u8 astc_pending; /* its ASTC levels decoded, their copies waiting in the batch (astc.c): vk_surf_use flushes it */
    /* the first level its views show: a big new ASTC texture is decoded from this level down, and drawn from it
       (blurrier), until the levels above are decoded at the next presents (texture.c astc_spread). 0 when whole */
    u8 astc_base;
    u8 prefetched; /* 1: made ahead of its first draw (nvn_res.c rp_prefetch) and not drawn yet; 2: drawn since */
    struct Surf* next;
    struct Surf* rt_next; /* real render targets, for overlapping guest-memory writes */
} Surf;
/* resolution= in the .ini: render targets are made this much bigger (read once, at startup) */
extern float g_vk_scale;
u32 vk_sc(u32 v);      /* a size or coordinate scaled, at least 1 */
float vk_scf(float v); /* the same for viewport values */
/* w in pixels; pitch: bytes per row of a pitch-linear target (the register holds it in place of the width) */
Surf* vk_rt(u64 gva, u32 w, u32 h, u32 fmt, u32 tile, int depth, u32 pitch);
Surf* vk_rt_array(u64 gva, u32 w, u32 h, u32 fmt, u32 tile, u32 pitch, u32 layers, u64 layer_size, int volume);
Surf* vk_zt_array(u64 gva, u32 w, u32 h, u32 fmt, u32 tile, u32 layers, u64 layer_size);
Surf* vk_rt_find(u64 gva);
Surf* vk_rt_snapshot(Surf* rt);                  /* copy a target so a draw can sample its contents while writing it */
Surf* vk_rt_for_cpu(u64 cpu_addr, u32 w, u32 h); /* the render target the guest presents from that memory */
/* a render target read where its guest memory is (a texture, a present): reload it if the GPU wrote its memory */
void vk_rt_note_read(Surf* s);
void vk_rt_before_write(u64 gva, u64 size);
/* an image of a scaled target's format at the guest's size with that many layers, a transfer's source and
   destination: to move it to and from guest memory, or into a texture a blit can't reach. VK_NULL_HANDLE on failure */
VkImage vk_unscaled_image(const Surf* s, u32 layers, VmaAllocation* mem);
/* moves a surface into a layout for a use, with the barrier that needs (which ends an open rendering pass) */
void vk_surf_use(VkCommandBuffer cb, Surf* s, VkImageLayout layout, VkPipelineStageFlags2 stage, VkAccessFlags2 access);
/* the stages a texture moved to SHADER_READ_ONLY_OPTIMAL is made visible to: all that sample, since a later read in
   the same layout gets no barrier of its own */
#define VK_SAMPLED_STAGES (VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT)
VkImageView vk_rt_attachment(Surf* s, int srgb_write);
void vk_rt_flush(u64 gva);
void vk_gfx_image_mem_need(u64 gva, u64 size);
void vk_gfx_image_land(void);
/* graphics storage images at a barrier (gpu_barrier_point): each pending one is copied for writeback after the work so
   far and submitted, and merges once that submission is done (vk_gfx_image_frame_done, from land_through) or sooner
   when read (vk_gfx_image_mem_need, vk_gfx_image_land). copied_need lands only those already copied, so a reader in a
   draw's setup (buffer fetches) waits for a submitted copy at most, never submits */
void vk_gfx_image_copy(void);
void vk_gfx_image_copied_need(u64 gva, u64 size);
void vk_gfx_image_frame_done(u64 value);
void vk_gfx_image_back_reset(u64 last); /* frame_recycle: last's back ring resets */
u64 vk_surf_guest_size(const Surf* s);
int vk_surf_can_writeback(const Surf* s);
void vk_surf_flush(Surf* s, const u8* original); /* original preserves newer guest writes for storage images */
/* vk_surf_flush in two halves, for a writeback that lands after its submission is done: the copy of s's rows into
   buf at off (vk_surf_back_size bytes) after the work recorded so far, 0 when s has nothing to write back; then, once
   that submission is done and s not drawn to since, the merge of p (the host's view of buf at off) */
u64 vk_surf_back_size(const Surf* s);
int vk_surf_copy_back(Surf* s, VkBuffer buf, u64 off);
void vk_surf_merge_back(Surf* s, const u8* p, const u8* original);
void vk_tex_note_writeback(Surf* s);
void vk_rt_drawn(Surf* s);      /* a render target was drawn to (gpu/vk/render.c) */
void vk_rt_drawn_full(Surf* s); /* a full clear or blit replaced every pixel */
int vk_rt_drawn_fresh(Surf* s); /* no guest-memory write has replaced the target since its last draw */
int vk_rt_drawn_in(u64 lo, u64 hi, Surf** out, int max); /* count drawn in [lo, hi), write up to max */
void vk_rt_reload(u64 gva);
/* textures share the surface table */
Surf* vk_surf_first(u64 gva);
void vk_surf_add(Surf* s);
VkImageAspectFlags vk_aspect(const VkFmt* f);
void vk_surf_evict(u32 before); /* textures and render targets last used before that present */
int vk_blit2d(const Surf2D* s, const Surf2D* d, int dx, int dy, int dw, int dh, double sx, double sy, double sdx,
              double sdy);

/* astc.c: decode tightly packed ASTC blocks and copy to a level/layer of t.
   Caller ends rendering and transitions t to TRANSFER_DST_OPTIMAL first. */
void vk_astc_upload(VkCommandBuffer cb, Surf* t, const u8* blocks, u32 w, u32 h, u32 depth, u32 level, u32 layer);
/* the encodes and copies of the strips vk_astc_upload decoded since the last flush, outside rendering passes. Runs
   when a waiting texture is first used (vk_surf_use), freed, or the commands are submitted (vk_submit) */
void vk_astc_flush(VkCommandBuffer cb);

/* vic.c: the VIC composition as a compute shader, synchronously: 1 when it composed the picture into the job's
   output planes, 0 when the job is too large for the GPU and the caller composes in guest memory. Called without
   vk_lock, which it takes only around what needs it */
int vk_vic_compose(const VicJob* j);

/* texture.c */
/* SNX_NVN_FRONTEND=2 (gpu/vk/nvn_res.c): the host's image of the window buffer at cpu_addr as a Surf for a present
   to show, NULL when the host has none. Under vk_lock */
Surf* vk_nvn_live_surf(u64 cpu_addr);
Surf* vk_tex_from_tic(const u32* tic);
Surf* vk_tex_from_tic_raw(const u32* tic); /* UINT view of raw RGBA8 texel bytes */
void vk_tex_invalidate_all(void);          /* force the next sampled-texture lookup to check guest bytes */
/* a view of s as the shader declares it, sRGB or linear (color formats that have both), with a component mapping;
   VK_NULL_HANDLE when the image can't be viewed that way */
/* stencil: a depth/stencil surface's stencil aspect instead of its depth */
VkImageView vk_tex_view(Surf* s, VkImageViewType type, int srgb, const VkComponentMapping* map, int stencil);
/* an empty texture of a view type: num 0 float, 1 unsigned, 2 signed integer, 3 depth at 1.0 (shadow samplers) */
VkImageView vk_null_view(VkImageViewType type, int num);
VkImageView vk_one_view(VkImageViewType type, int num); /* the same, every component 1 (debugging) */
/* the sampler for a TSC: raise lets anisotropy= raise its filtering (a 2D texture with mipmaps), integer gives its
   border color as integers (an integer texture) */
VkSampler vk_sampler(const u32* tsc, int raise, int integer);
extern float g_vk_lod_bias;  /* texture_lod_bias=, read once at start, for the same samplers as anisotropy */
extern int g_vk_anisotropy;  /* anisotropy=, read once at start: the sampler table keeps the samplers it made */
void vk_tex_frame(void);     /* a frame was presented: evicts what wasn't used for a while */
void vk_tex_forget(Surf* s); /* s goes: out of the queue of textures whose top levels wait to be decoded */

/* gpu/vk/buffer.c: guest memory [gva, gva + len) as a buffer the GPU reads, valid until the next submit */
int vk_buf_get(u64 gva, u64 len, VkBuffer* buf, u64* off);
int vk_buf_get_strict(u64 gva, u64 len, VkBuffer* buf, u64* off); /* no delayed content checks */
/* a copy of len bytes made now (data that changes within a frame), or of zeros when gva is 0 */
int vk_buf_copy(u64 gva, u64 len, VkBuffer* buf, u64* off);
int vk_buf_copy_writable(u64 gva, u32 len, VkBuffer* buf, u64* off);
typedef struct {
    u64 addr;
    u32 size;
    int writable, aliased;
    VkBuffer buf;
    u64 off, range;
} VkDrawGbuf;
typedef struct {
    VkDrawGbuf b[2][16];
    int n[2];
    VkBuffer map_buf[2];
    u64 map_off[2], map_size[2];
} VkDrawStorage;
int vk_buf_draw_has_alias(const VkDrawStorage* draw);
int vk_buf_draw_prepare(VkDrawStorage* draw);
void vk_buf_writable_abort(void);
void vk_buf_writable_commit(void);
void vk_buf_mem_need(u64 gva, u64 len);
int vk_buf_pending(u64 gva, u64 len); /* a pending graphics result overlaps the range */
int vk_buf_pending_any(void);         /* any graphics result is pending */
int vk_buf_pending_unsubmitted(void); /* one is in the commands recorded since the last submit */
/* release.c: releases the game can observe, once the results before them land (renderer.h release_write) */
void vk_release_write(u64 gva, const void* data, u32 len);
void vk_release_incr(u32 id);
void vk_release_host_gate(u64 gate); /* the NVN host path's work to wait for: the submission that holds it */
/* a samples-passed report: len bytes at gva (4, or 16 with ticks after the count), the count of query.c's counter's
   segments [first, end), once the GPU has done the submit being recorded */
void vk_release_samples(u64 gva, u32 len, u32 counter, u64 first, u64 end, u64 ticks);
void vk_release_submit(void); /* the GPFIFO submission ended or waits: a release gated on the recording submits */
void vk_release_need(u64 gva, u64 len);
u32 vk_release_value(u32 id);
void vk_release_retire(u32 id);
void vk_release_drain(void);
/* query.c: the samples-passed counter with occlusion queries (renderer.h samples_*), under vk_lock. vk_query_draw before
   a game's draw in a rendering pass, vk_query_pass_end before the pass ends or a draw of the renderer's own in it
   (render.c), vk_query_flush outside a pass before a submit (cmd.c) */
void vk_query_draw(VkCommandBuffer cb, u32 chan, u32 msaa_mode); /* msaa_mode: 3D 0x574 */
void vk_query_pass_end(VkCommandBuffer cb);
void vk_query_flush(VkCommandBuffer cb);
void vk_query_reset(u32 chan);
void vk_query_enable(u32 chan, int on);
int vk_query_report(u32 chan, u64 gva, u32 len, u64 ticks);
/* a report performed (release.c): the count of counter's segments [first, end) */
u64 vk_query_value(u32 counter, u64 first, u64 end);
void vk_buf_mem_need_map(u64 gva, u64 len);
void vk_buf_before_write(u64 gva, u64 len);
void vk_buf_land(void);
void vk_buf_land_syncwait(void);
void vk_buf_land_map(void);
void vk_buf_frame_done(u64 frame);
void vk_buf_host_barrier(VkCommandBuffer cb, u64 frame);
void vk_buf_frame(void);
/* Land GPU-written bytes only where guest memory still matches the shader's input snapshot. */
void vk_result_begin(void);
void vk_result_end(void);
int vk_result_landing(void);
void vk_write_changed(u64 addr, const u8* res, const u8* org, u32 size);
/* 1 if [host, host+len) was written since the write-tracking clock read `since`, 0 if not, -1 when write tracking
   can't tell (no host pointer, or not available): then check the contents */
int vk_written_since(const u8* host, u64 len, u32 since);
void vk_written_each(const u8* host, u64 len, u32 since, void (*fn)(int, u64, u64), int arg);
u32 vk_ww_now(void);
void vk_mem_changed(u64 cpu, u64 len); /* gpu.c's g_gpu_mem_changed: the range counts as written */

/* pipeline.c */
/* the constant buffer words a translation read (the translator resolves BRX jump tables through them): what it made
   is only reused while they still hold the same */
typedef struct {
    u32 idx, off, ok, v;
} VkCbRead;
typedef struct {
    VkCbRead* r;
    u32 n;
} VkCbReads;
typedef int (*VkCbReader)(u32 idx, u32 off, u32* out);
u32* vk_translate_spirv(const u8* code, u32 max_bytes, int stage, ShaderInfo* si, size_t* words, VkCbReader read,
                        VkCbReads* reads);
/* why the device can't take the module: a capability it declares (Float64, Int64) whose feature is off, or NULL */
const char* vk_spirv_unavailable(const u32* spv, size_t words);
u64 vk_shader_hash(const u8* code, u32 max_bytes, u32* out_len, VkCbReader read); /* BRX tables through read */
/* where each new shader's snx-shaderc corpus entry goes: SNX_SHADER_CORPUS, else with diag=1 captures/shaders beside
   the games; NULL for neither */
const char* vk_corpus_dir(void);
int vk_cb_reads_hold(const VkCbReads* reads, VkCbReader read);
typedef struct VkSh {
    u64 hash;
    u32 mask[20]; /* vk_shader_get's masks, including the geometry passthrough skip mask */
    int stage;
    VkCbReads cbr;
    ShaderInfo si;
    VkShaderModule mod;
    int failed;
    u32 clen, clen_a;
    u64 qh, qh_a;
    struct VkSh* next;
} VkSh;
extern VkPipelineLayout g_vk_playout;
extern int g_vk_preserve_infnan;
extern int g_vk_preserve_infnan64;
extern int g_vk_flush_denorm;
extern VkShaderStageFlags g_vk_subgroup_stages;
extern VkDescriptorSetLayout g_vk_dslayout[5]; /* by set: shader_vk_set(stage) */
extern u32 g_vk_stage_sets; /* sets in the pipeline layout: 5, or 2 without the tessellation and geometry stages */
/* the stages that read the push constants: every graphics stage reads the viewport/depth parameters in pc[0..3]
   (the last one before rasterization applies them); fragment reads the alpha test in pc[4..6] and sprite T flip
   in pc[7] */
#define VK_PUSH_STAGES                                                                                                 \
    (VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT |                                           \
     VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT | VK_SHADER_STAGE_GEOMETRY_BIT | VK_SHADER_STAGE_FRAGMENT_BIT)
void vk_pipeline_init(void);
/* the shader at gva (after a VertexA program at gva_a, or 0). The masks key integer inputs: attributes for the
   vertex stage, textures and render target outputs for the fragment stage. rt_int keys the primitives of the
   tessellation evaluation and geometry stages (ShaderInfo.rt_int_mask). clip: the clip distances the 3D engine
   enables, and SNX_SH_NOT_LAST (the stages before rasterization). The translator reads ch's bound constant buffers
   (ch->cbind) to resolve indirect branches */
VkSh* vk_shader_get(const GpuChannel* ch, u64 gva, u64 gva_a, int stage, u64 int_mask, u64 sint_mask, u32 rt_int,
                    u32 rt_sint, u32 clip, u64 dc_color, u32 dc_funcs, u32 unfed);
typedef struct {
    VkShaderModule vs, fs;
    VkShaderModule tcs, tes, gs; /* tessellation control and evaluation, geometry: VK_NULL_HANDLE when off */
    u32 patch;                   /* control points of a patch, with tessellation */
    /* POINT_LIST, LINE_LIST, TRIANGLE_LIST, their adjacency forms or PATCH_LIST: the topology is dynamic within its
       class */
    u32 topo_class;
    u32 polygon;
    u32 depth_clamp;
    u32 nattr, nbind;
    struct {
        u32 loc, binding, format, offset;
    } attr[32];
    struct {
        u32 divisor; /* 0: per vertex */
    } bind[33];      /* bindings 0..nbind-1: a binding per attribute at most, and one for constant attributes */
    u32 ncolor;
    u32 color[8];
    u32 depth, stencil;
    struct {
        u32 en, sc, dc, cop, sa, da, aop, mask;
    } blend[8];
} PipeKey;
VkPipeline vk_pipeline_get(const PipeKey* k);
VkPipeline vk_pipeline_make(const PipeKey* k);
/* since the last read: libraries compiled, by part (vertex input, stages before rasterization, fragment, output), their
   ns and the longest; links, their ns and the longest */
typedef struct {
    u64 n[4], ns[4], max[4], links, link_ns, link_max;
} VkLibStats;
void vk_pipeline_lib_stats(VkLibStats* s);
/* a vertex (or else fragment) module compiled as its library into the pipeline cache, and the library destroyed */
void vk_pipeline_prep(VkShaderModule m, int vertex);
const char* vk_pipeline_cache_dir(void);
void vk_pipeline_mark_dirty(void); /* the next save writes the cache */
int vk_cpu_threads(void);
void vk_thread_low(void); /* the calling thread runs below the game's */
extern u64 g_vk_astc_px;
extern int g_vk_astc_bc; /* format.c: ASTC textures are stored as BC3 (device.c decides) */
/* SNX_NVN_TEX_BUDGET: the ASTC texels a present may decode for textures seen for the first time, 0 for no limit; over
   it their first upload waits for the next present, and vk_tex_from_tic gives NULL with g_vk_tex_later set */
extern u64 g_vk_tex_budget;
extern int g_vk_tex_later;
extern u64 g_vk_tex_deferred;
/* where a frame's texture cache misses go, ns per part (nvn_res.c's slow frame line reads and clears them): guest
   memory the GPU wrote landed first, vk_lock, the whole vk_tex_from_tic, and within it image creation, the hash and
   write tracking of a new texture, its upload, and render targets' texels copied in; then the view and the sampler */
enum {
    VK_TP_LAND,
    VK_TP_LOCK,
    VK_TP_TIC,
    VK_TP_CREATE,
    VK_TP_HASH,
    VK_TP_UPLOAD,
    VK_TP_DRAWN,
    VK_TP_VIEW,
    VK_TP_SAMPLER,
    VK_TP_END,
    VK_TP_READ,
    VK_TP_ROWS,
    VK_TP_ASTC,
    VK_TP_STAGE,
    VK_TP_SALLOC,
    VK_TP_SBYTES,
    VK_TP_N
};
/* the upload's: render pass end, guest reads and buffers, deswizzle, ASTC staging and dispatch, other staging and
   conversion, of which the staging allocation; SBYTES counts bytes staged, not ns */
extern u64 g_vk_tex_ns[VK_TP_N];
/* textures made ahead of their first draw: set while nvn_res.c makes one; how many of them a draw found later
   (hits), and how many had changed in memory before that (stale, loaded twice); the guest bytes of those not drawn
   yet, which eviction keeps up to SNX_NVN_PREFETCH_MB (surface.c vk_surf_evict) */
extern int g_vk_prefetching;
extern u64 g_vk_prefetch_hits, g_vk_prefetch_stale, g_vk_prefetch_kept;
/* textures made at a draw instead, by when the game registered them: this present, 1-29 presents before, 30 or more
   before, never */
extern u64 g_vk_made_at_draw[4];
VkPipelineCache vk_pipeline_cache(void); /* for pipelines made elsewhere (compute) */
void vk_pipeline_save(int last);
VkShaderModule vk_module_spv(const u32* words, size_t count);
VkShaderModule vk_module_spv_location(const u32* words, size_t count, u32 location);

/* renderer.c: the next present of the buffer at cpu_addr saved as path.ppm (the NVN replay's comparison) */
void vk_capture_next_present(u64 cpu_addr, const char* path);

/* compute.c */
void vk_dispatch(GpuChannel* ch, const u32* q, u64 code);
void vk_compute_land(void); /* pending compute results into guest memory, now */
/* the compute results so far, submitted and put aside to land later: the batch to wait for (0: none pending) */
u64 vk_compute_put_aside(void);
void vk_compute_wait(u64 value);                 /* until the compute batch is done (any thread, without vk_lock) */
void vk_compute_land_through(u64 value);         /* the batches up to it, done, land (vk_lock) */
void vk_compute_before_write(u64 gva, u64 size); /* gpu.c's g_gpu_before_write */
void vk_compute_mem_need(u64 a, u64 n);          /* a read of [a, a + n): land results there first */
void vk_compute_mem_need_map(u64 a, u64 n);      /* the GMMU changes [a, a + n): land results there first */
/* a pending compute result covering [gva, gva + len): the arena buffer and offset to read it from on the GPU */
int vk_compute_gpu_src(u64 gva, u64 len, VkBuffer* buf, u64* off);
extern VkSemaphore g_vk_csem;  /* timeline semaphore the compute queue signals */
extern u64 g_vk_cs_wait_value; /* the value graphics work waits for before reading compute results */
extern u64 g_vk_dispatches, g_vk_cs_batches, g_vk_cs_skipped, g_vk_cs_wait_ns; /* statistics */

/* render.c */
void vk_render_end(void); /* ends the dynamic rendering pass, if one is open */
/* the on-screen controls and the game's menu over a swapchain image in TRANSFER_DST_OPTIMAL, which it stays in;
   safe is the picture's part clear of the system's bars (gpu/vk/ui.c) */
void vk_ui_draw(VkCommandBuffer cb, VkImage sc, const int safe[4]);
void vk_ui_swapchain_gone(void); /* its images' views go before the swapchain does */
void vk_render_clear(GpuChannel* ch, u32 v);
VkViewport vk_render_viewport(const u32* r);
int vk_render_depth_clamp(const u32* r);
VkFrontFace vk_render_front_face(const u32* r);
void vk_render_draw_texture(GpuChannel* ch);
void vk_render_draw(GpuChannel* ch, int indexed, u32 topology, u32 first, u32 count, u32 base_vertex, u32 base_instance,
                    u32 instances);
/* a texture binding, for draws and compute: the view type a declaration takes, the view of t with the header's
   swizzle (VK_NULL_HANDLE when t is NULL or not of the number type num: 0 float, 1 unsigned, 2 signed integer), and
   the header's sampler with the declaration's comparison (integer textures unfiltered). The sampler's t and want,
   the texture whose own view is bound (NULL for none) and its view type, say whether anisotropy= may raise it */
VkImageViewType vk_tex_view_type(const ShTex* st);
VkImageView vk_tex_bind_view(Surf* t, VkImageViewType want, const u32* tic, const u32* tsc, int num);
VkSampler vk_tex_bind_sampler(const u32* tsc, const ShTex* st, int num, const Surf* t, VkImageViewType want);
extern u64 g_vk_drawn, g_vk_skipped; /* draws recorded, and draws skipped (unsupported topology, failed shader) */

#endif
