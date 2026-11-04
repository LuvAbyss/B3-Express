/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* NVN memory pools as Vulkan buffers over the guest's storage (nvn_res.c), in plain types for hle/nvn */
#ifndef SNX_GPU_VK_NVN_RES_H
#define SNX_GPU_VK_NVN_RES_H
#include "common/cpu/aarch64/cpu.h"

/* guest storage [cpu, cpu + size) imported as a Vulkan buffer (VK_EXT_external_memory_host): its number, or -1 when
   it can't be (no extension, misaligned, refused). Any thread */
int vk_nvn_pool_import(u64 cpu, u64 size);
/* n bytes at off of imported buffer which copied on the GPU and compared with guest memory: 1 equal, 0 not, -1 when
   the check couldn't run. Takes vk_lock to submit */
int vk_nvn_pool_check(int which, u64 off, u32 n);
u64 vk_nvn_import_align(void); /* the alignment imports need, 0 without the extension */
void vk_nvn_pool_gpu(int which, u64 gpu); /* the GPU address of an imported pool, for bindings by GPU address */
void vk_nvn_pool_mirror(int which, u64 flags); /* an imported pool whose NVNmemoryPoolFlags ask the GPU to cache it */
/* The key a stage is translated for, as the renderer's vk_shader_get takes it: vertex, the attributes of integer
   formats (int_mask) and the signed ones; fragment, the integer render targets (rt_int, rt_sint) and the inputs the
   last stage before rasterization doesn't write (unfed), the textures of integer formats (tex_int, tex_sint) and the
   colour textures sampled with a depth compare (dc_mask, their functions in dc_funcs); clip, the clip distances that
   clip or SNX_SH_NOT_LAST */
typedef struct {
    u32 int_mask, sint_mask, rt_int, rt_sint, clip, unfed;
    u64 tex_int, tex_sint, dc_mask;
    u32 dc_funcs, pad;
} NvnStageKey;
/* A program stage (gpu/shader stage number ST_*) at code_gva, window bytes of it, translated live (compute_stub: 80
   zero bytes in front, compute having no program header), its constant buffer 1 at cb1_gva; cs_local (3 words) and
   shared for compute; key NULL for none. The module's number, or -1 with the reason in why. Takes vk_lock */
int vk_nvn_shader(int stage, u64 code_gva, u32 window, int compute_stub, u64 cb1_gva, u32 cb1_size,
                  const u32* cs_local, u32 shared, const NvnStageKey* key, char* why, u32 why_size);
/* what a module's translation found: generic inputs read, outputs written, and whether the fragment program header
   declares colour outputs (sph word 18). 0 for no such module */
int vk_nvn_shader_io(int mod, u32* in_mask, u32* out_mask, u32* frag_outputs);
/* a vertex, fragment or compute stage the game gave a program: prepared by the workers before a draw or dispatch
   needs it. cs_local (the workgroup size) and shared are a compute stage's, NULL and 0 for the others */
void vk_nvn_prep_add(int stage, u64 code_gva, u32 window, u64 cb1_gva, u32 cb1_size, const u32* cs_local, u32 shared);

/* A draw's pipeline in NVN's terms, from the bound state objects (decoded through the driver's getters) and the
   program bound to each stage. Zeroed before it is filled: it is a cache key */
typedef struct {
    int mod[5]; /* modules for vertex, tessellation control and evaluation, geometry, fragment; -1 for none */
    u32 primitive;   /* NVNdrawPrimitive */
    u32 patch;       /* SetPatchSize, with tessellation */
    u32 polygon;     /* NVNpolygonMode: 0 point, 1 line, 2 fill */
    u32 depth_clamp; /* SetDepthClamp */
    u32 samples;     /* the multisample state's */
    u32 logic_op;    /* the colour state's NVNlogicOp (3 copy) */
    u32 nattr;
    u32 attr_format[16], attr_offset[16], attr_stream[16]; /* NVNformat 0: the attribute is off */
    u32 nstream;
    u32 stream_divisor[16];
    u32 ncolor;
    u32 color_format[8]; /* NVNformat of each target (a view's when it sets one), 0 for none */
    u32 depth_format;
    u32 blend_on;        /* the colour state's blend enable per target */
    u8 blend[8][6];      /* per target: src and dst RGB, src and dst alpha (NVNblendFunc), RGB and alpha equation */
    u32 chanmask;        /* r, g, b, a write per target, 4 bits each */
} NvnPipeDesc;
/* the Vulkan pipeline for d in the renderer's layout (g_vk_playout) through its own vk_pipeline_get: 1 when there is
   one (*made: it was created now), 0 with the reason in why. Takes vk_lock to create */
int vk_nvn_pipeline(const NvnPipeDesc* d, int* made, char* why, u32 why_size);
/* the same for a compute module */
int vk_nvn_compute_pipeline(int mod, int* made, char* why, u32 why_size);
/* ------------------------------------------------------------ descriptors (part 2c) */
typedef struct {
    u64 addr, size;
} NvnRange;
/* what one stage of a draw binds, in NVN's terms */
typedef struct {
    int nvn_stage;     /* NVNshaderStage: 0 vertex, 1 fragment, 2 geometry, 3 tess control, 4 tess eval, 5 compute */
    u64 cb1, cb1_size; /* the program's constants, its constant buffer 1 (GPU address) */
    NvnRange ubo[16], ssbo[16];
    u64 tex[32], img[8]; /* bound handles */
    u64 tex_pool, tex_count, smp_pool, smp_count; /* CPU addresses of the pools' descriptors, and how many */
} NvnStageBind;
enum { NVN_B_CB0, NVN_B_CB1, NVN_B_CB2, NVN_B_UBO, NVN_B_UBO_ZERO, NVN_B_SSBO, NVN_B_TEX, NVN_B_IMG, NVN_B_KINDS };
/* every binding module mod reads, resolved as b binds them: constant buffers 0 and 2 made on the host (storage buffer
   addresses, bound texture handles), 1 the program's, 3 and up uniform buffers; storage buffers by their place in
   constant buffer 0; textures by handle through the pools' TIC and TSC (vk_tex_from_tic). Counts each binding into
   ok or fail by kind (NVN_B_*), each failure's reason through why. The number of failures. Takes vk_lock */
u32 vk_nvn_stage_check(int mod, const NvnStageBind* b, u64* ok, u64* fail, void (*why)(const char*));
/* the fragment key's texture masks for module mod's textures as b binds them */
void vk_nvn_tex_key(int mod, const NvnStageBind* b, NvnStageKey* key);
void vk_nvn_live_fence(void); /* nvnCommandBufferFenceSync in a list */
void vk_nvn_frame(void);      /* a present: textures are looked up again */
void vk_nvn_slots_dump(void); /* logs where in which constant buffer the modules found handles and buffer addresses */
/* ------------------------------------------------------------ replay of one frame (part 3) */
typedef struct {
    u64 gpu;       /* the texture's GPU address: what textures sampled later in the frame find it by */
    u32 w, h;      /* the texture's size */
    u32 format;    /* the NVNformat drawn as (a view's when it sets one) */
    u32 layers;    /* the texture's (an array's depth), and the layer the view draws to */
    u32 layer;
} NvnTarget;
/* the dynamic state of a draw, in NVN's terms */
typedef struct {
    i32 viewport[4]; /* x, y, w, h */
    float depth_range[2];
    i32 scissor[4];  /* x, y, w, h; w < 0 when never set */
    u32 upper_left;  /* nvnDeviceSetWindowOriginMode: 1 upper left */
    u32 depth_zero;  /* nvnDeviceSetDepthMode: 1 near is zero */
    u32 cull, front_ccw, offset_enables;
    u32 depth_test, depth_write, depth_func, stencil_test;
    u32 stencil_func[2], stencil_op[2][3]; /* front and back: fail, depth fail, pass (NVN enums) */
    u32 stencil_ref[2], stencil_wmask[2], stencil_vmask[2];
    float offset[3]; /* polygon offset factor, units, clamp */
    float blend_color[4], line_width, alpha_ref;
    u32 alpha_func; /* the colour state's NVNalphaFunc */
    NvnRange vbuf[16];
    u32 stride[16];
} NvnDrawDyn;
typedef struct {
    int kind; /* 0 DrawArrays, 1 DrawElements(Instanced), 2 DrawElementsIndirect, 3 MultiDrawElementsIndirectCount */
    u32 primitive, index_type, count, first, instances, base_instance, max_draws, stride;
    i32 base_vertex;
    u64 index, indirect, parameter; /* GPU addresses */
} NvnDrawCall;
/* SNX_NVN_REPLAY: one frame's draws recorded into Vulkan commands of the replay's own, into render targets of its
   own (images by texture address, sampled in their place later in the frame). Draws that would write guest-visible
   memory (storage images, compute) are left out and counted. Replay end submits after the renderer's commands so far,
   reads the presented texture back as out.ppm and has the renderer save its own present of the buffer at
   present_cpu as out-renderer.ppm. Takes vk_lock where it touches the renderer */
int vk_nvn_replay_begin(void);
void vk_nvn_replay_targets(const NvnTarget* color, u32 n, const NvnTarget* depth);
/* bind: the five graphics stages' bindings (vertex, tess control, tess eval, geometry, fragment), those with
   d->mod[s] >= 0 used */
void vk_nvn_replay_draw(const NvnPipeDesc* d, const NvnStageBind* bind, const NvnDrawDyn* dyn,
                        const NvnDrawCall* call);
/* a compute dispatch (x, y, z groups, or its arguments at the GPU address indirect when that isn't 0); what it writes
   goes to the replay's copies of the ranges */
void vk_nvn_replay_dispatch(int mod, const NvnStageBind* b, u32 x, u32 y, u32 z, u64 indirect);
void vk_nvn_replay_buffer(u64 src, u64 dst, u64 size, u32 value); /* copy (src) or clear (src 0) of guest buffers */
/* UpdateUniformBuffer: size bytes at the guest buffer dst, in order with the list's draws. Real writes only: otherwise
   the stream that runs writes them */
void vk_nvn_replay_update(u64 dst, const void* data, u32 size);
void vk_nvn_replay_clear_color(u32 index, const float* rgba, u32 mask, const NvnDrawDyn* dyn);
void vk_nvn_replay_clear_depth(float depth, int depth_mask, u32 stencil, u32 stencil_mask, const NvnDrawDyn* dyn);
void vk_nvn_replay_skip(const char* why); /* a command the replay leaves out */
void vk_nvn_replay_end(u64 present_gpu, u64 present_cpu, const char* out);
/* SNX_NVN_LIVE (phase 4 step 4.1): the replay's commands every frame, into the renderer's command buffer, in images
   kept from frame to frame. live_window(1) takes vk_lock and ends the renderer's pass; every replay command of a
   submitted list goes between it and live_window(0). Written ranges still go to copies, so guest memory is the
   renderer's */
int vk_nvn_live_start(void);
void vk_nvn_live_window(int open);
/* a present of the texture at GPU address gpu. out not NULL: that image is read back as out.ppm (waits for the GPU),
   and the renderer's next present of the buffer at cpu as out-renderer.ppm; all: every image as out-tNN.ppm too */
void vk_nvn_live_present(u64 gpu, u64 cpu, const char* out, int all);
void vk_nvn_live_tex_free(u64 gpu); /* nvnTextureFinalize: the image kept for that address goes */
/* nvnTexturePoolRegisterTexture (game thread): the texture at gpu can be drawn from now. A measure for decoding ahead:
   vk_nvn_tex_known gives the present it was first registered at, or -1 when it wasn't */
void vk_nvn_tex_registered(u64 gpu);
long long vk_nvn_tex_known(u64 gpu);
void vk_nvn_live_show(void); /* SNX_NVN_FRONTEND=2: vi's presents show the host's images (vk_nvn_live_surf) */
/* SNX_NVN_RELEASE_ONLY (step 4.3): what the frame writes goes to guest memory for real, each list goes to the GPU when
   its window closes, and the stream's releases wait for it (vk_release_host_gate) */
void vk_nvn_live_real(void);
void vk_nvn_window_size(u32 w, u32 h); /* the game's window textures: its final picture */
void vk_nvn_live_debug(int on); /* logs the host images each draw samples */
void vk_nvn_live_stats(u64 frames);
/* the CPU address of GPU address gpu in an imported pool, 0 when none holds it */
u64 vk_nvn_cpu_of(u64 gpu);
/* vertex attribute NVNformat: integer (1), signed integer (3), else 0; and whether a colour NVNformat is integer */
u32 vk_nvn_attr_int(u32 nvn_format);
u32 vk_nvn_color_int(u32 nvn_format);

#endif
