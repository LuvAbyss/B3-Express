/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* ASTC LDR uploads on the graphics queue. CPU work stops at deswizzling the
   compressed blocks. The shader writes packed RGBA8 to device memory, then a
   transfer copies those bytes to the image (including sRGB images, without an
   unwanted gamma conversion). Chunking bounds scratch memory and SSBO ranges. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nx/gpu/vk/vk.h"
#include "nx/gpu/texture/astc_spv.h"
#include "nx/gpu/vk/bc3_spv.h"
#include "nx/runtime/platform/log.h"

static VkDescriptorSetLayout g_astc_ds;
static VkPipelineLayout g_astc_layout;
static VkPipeline g_astc_pipe, g_bc_pipe;
static VkBuffer g_astc_scratch, g_bc_scratch; /* decoded texels; BC3 blocks of an image stored as BC3 */
static VmaAllocation g_astc_mem, g_bc_mem;
static u64 g_astc_cap;

/* astc.comp's push constants: the strip's size in texels, the block footprint, and whether to use the
   specification's sRGB decode mode, whose R, G and B bytes differ from the UNORM mode's. An SRGB image (the
   texture header's sRGB bit, vk_tic_format) gets the sRGB mode, so the bytes are those an sRGB ASTC texture
   decodes to on the console. */
typedef struct {
    u32 w, h, bw, bh, srgb;
} AstcParams;

static void astc_init(void) {
    if (g_astc_pipe) return;
    VkDescriptorSetLayoutBinding bindings[2] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
    VkDescriptorSetLayoutCreateInfo di = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = bindings};
    if (!VK_OK(vkCreateDescriptorSetLayout(g_vk.dev, &di, NULL, &g_astc_ds)))
        fatal("vulkan: cannot create the ASTC descriptor layout");
    VkPushConstantRange push = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(AstcParams)};
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                     .setLayoutCount = 1,
                                     .pSetLayouts = &g_astc_ds,
                                     .pushConstantRangeCount = 1,
                                     .pPushConstantRanges = &push};
    if (!VK_OK(vkCreatePipelineLayout(g_vk.dev, &li, NULL, &g_astc_layout)))
        fatal("vulkan: cannot create the ASTC pipeline layout");
    VkShaderModuleCreateInfo mi = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof astc_spv, .pCode = astc_spv};
    VkShaderModule module;
    if (!VK_OK(vkCreateShaderModule(g_vk.dev, &mi, NULL, &module))) fatal("vulkan: cannot load the ASTC shader");
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                                .module = module,
                                                .pName = "main"},
                                      .layout = g_astc_layout};
    int ok = VK_OK(vkCreateComputePipelines(g_vk.dev, vk_pipeline_cache(), 1, &ci, NULL, &g_astc_pipe));
    vkDestroyShaderModule(g_vk.dev, module, NULL);
    if (!ok) fatal("vulkan: cannot create the ASTC compute pipeline");
    if (g_vk_astc_bc) { /* the same set: texels in, blocks out, and the first two push constants */
        VkShaderModuleCreateInfo bi = {
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof bc3_spv, .pCode = bc3_spv};
        if (!VK_OK(vkCreateShaderModule(g_vk.dev, &bi, NULL, &module))) fatal("vulkan: cannot load the BC3 shader");
        ci.stage.module = module;
        ok = VK_OK(vkCreateComputePipelines(g_vk.dev, vk_pipeline_cache(), 1, &ci, NULL, &g_bc_pipe));
        vkDestroyShaderModule(g_vk.dev, module, NULL);
        if (!ok) fatal("vulkan: cannot create the BC3 compute pipeline");
    }
    /* Kept for the device lifetime, like the other internal pipelines. All uses
       are on g_vk.gfx, under vk_lock(), even when uploads originate in compute. */
    g_astc_cap = 16ull << 20; /* a strip of an image stored as BC3 takes whole fours of block rows */
    if (g_astc_cap > g_vk.props.limits.maxStorageBufferRange) g_astc_cap = g_vk.props.limits.maxStorageBufferRange;
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                             .size = g_astc_cap,
                             .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                             .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (!VK_OK(vmaCreateBuffer(g_vk.vma, &bi, &ai, &g_astc_scratch, &g_astc_mem, NULL)))
        fatal("vulkan: cannot allocate ASTC scratch memory");
    vk_mem_tag(g_astc_mem, VKM_STAGE);
    if (g_vk_astc_bc) {
        bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        if (!VK_OK(vmaCreateBuffer(g_vk.vma, &bi, &ai, &g_bc_scratch, &g_bc_mem, NULL)))
            fatal("vulkan: cannot allocate BC3 scratch memory");
        vk_mem_tag(g_bc_mem, VKM_STAGE);
    }
}

#define COMPUTE VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT
#define COPY    VK_PIPELINE_STAGE_2_COPY_BIT
#define SWRITE  VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT
#define SREAD   VK_ACCESS_2_SHADER_STORAGE_READ_BIT
#define TREAD   VK_ACCESS_2_TRANSFER_READ_BIT
/* Scratch reuse also needs a WAR dependency on the use before, including when that belongs to an earlier frame */
static void scratch_barrier(VkCommandBuffer cb, VkBuffer buf, VkPipelineStageFlags2 from, VkAccessFlags2 was,
                            VkPipelineStageFlags2 to, VkAccessFlags2 next) {
    VkBufferMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
                                .srcStageMask = from,
                                .srcAccessMask = was,
                                .dstStageMask = to,
                                .dstAccessMask = next,
                                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                .buffer = buf,
                                .size = VK_WHOLE_SIZE};
    VkDependencyInfo d = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .bufferMemoryBarrierCount = 1, .pBufferMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cb, &d);
}

u64 g_vk_astc_px; /* texels decoded */
/* SNX_ASTC_DUMP=dir (a test): each 2D level's blocks as the decoder gets them, <n>_<w>x<h>_<bw>x<bh>_<srgb>.astc, up
   to 1 GiB, for the decoder's offline check and benchmark (game data: it stays on this machine) */
static void astc_dump(const u8* blocks, u32 w, u32 h, u32 bw, u32 bh, int srgb) {
    static const char* dir;
    static int checked;
    static u32 n;
    static u64 total;
    char path[600];
    u64 size = (u64)((w + bw - 1) / bw) * ((h + bh - 1) / bh) * 16;
    FILE* f;
    if (!checked) {
        checked = 1;
        dir = getenv("SNX_ASTC_DUMP");
    }
    if (!dir || total + size > (1ull << 30)) return;
    snprintf(path, sizeof path, "%s/%05u_%ux%u_%ux%u_%d.astc", dir, n++, w, h, bw, bh, srgb);
    if (!(f = fopen(path, "wb"))) return;
    fwrite(blocks, 1, (size_t)size, f);
    fclose(f);
    total += size;
}
/* A batch of decoded strips, each in its own part of the scratch buffers, whose BC3 encodes and copies wait for
   vk_astc_flush (tex_upload calls it once a texture). On NVIDIA every barrier between two dispatches drains the GPU,
   so a texture's levels have none between them: one before the encodes, and one before the copies, which also moves
   the image to TRANSFER_DST. The scratch buffers are rings: their earlier readers are waited for once a command
   buffer and when a ring wraps, not once a level. A level waiting for the one before cost 71 us of GPU time, 70% of a
   load's decoding (3,815 levels, 272 of 395 ms); batched, the same levels take 170 ms */
#define BATCH 512
static struct {
    Surf* surf;
    u32 level, layer, w, height, bx, by;
    i32 y, z;
    u64 aoff, boff;
    AstcParams p;
} g_piece[BATCH];
static u32 g_npiece, g_bc_batch;
static u64 g_apos, g_bpos;            /* the rings' next free byte */
static u64 g_ring_sub = ~0ull;       /* the submission being recorded when the readers were last waited for */
static u64 align_up(u64 v) { return (v + 255) & ~(u64)255; }
static void set_write(VkDescriptorSet set, VkBuffer a, u64 aoff, u64 asize, VkBuffer b, u64 boff, u64 bsize) {
    VkDescriptorBufferInfo buffers[2] = {{a, aoff, asize}, {b, boff, bsize}};
    VkWriteDescriptorSet writes[2];
    for (u32 i = 0; i < 2; i++)
        writes[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                           .dstSet = set,
                                           .dstBinding = i,
                                           .descriptorCount = 1,
                                           .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                           .pBufferInfo = &buffers[i]};
    vkUpdateDescriptorSets(g_vk.dev, 2, writes, 0, NULL);
}
/* the earlier encodes and copies that read the rings, in this command buffer or one submitted before, are done
   before new decodes write them */
static void ring_wait(VkCommandBuffer cb) {
    scratch_barrier(cb, g_astc_scratch, COPY | COMPUTE, TREAD | SREAD, COMPUTE, SWRITE);
    if (g_bc_scratch) scratch_barrier(cb, g_bc_scratch, COPY, TREAD, COMPUTE, SWRITE);
    g_ring_sub = vk_submit_number();
}
void vk_astc_flush(VkCommandBuffer cb) {
    u32 i;
    if (!g_npiece) return;
    for (i = 0; i < g_npiece; i++) g_piece[i].surf->astc_pending = 0; /* before the vk_surf_use calls below */
    /* the decodes before the encodes (or the copies) */
    scratch_barrier(cb, g_astc_scratch, COMPUTE, SWRITE, g_bc_batch ? COMPUTE : COPY, g_bc_batch ? SREAD : TREAD);
    if (g_bc_batch) {
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_bc_pipe);
        for (i = 0; i < g_npiece; i++) {
            VkDescriptorSet es = vk_desc_alloc(g_astc_ds);
            if (!es) fatal("vulkan: cannot allocate BC3 descriptors");
            set_write(es, g_astc_scratch, g_piece[i].aoff, (u64)g_piece[i].w * g_piece[i].height * 4, g_bc_scratch,
                      g_piece[i].boff, (u64)g_piece[i].bx * g_piece[i].by * 16);
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_astc_layout, 0, 1, &es, 0, NULL);
            vkCmdPushConstants(cb, g_astc_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof g_piece[i].p, &g_piece[i].p);
            vkCmdDispatch(cb, (g_piece[i].bx + 31) / 32, g_piece[i].by, 1);
        }
        scratch_barrier(cb, g_bc_scratch, COMPUTE, SWRITE, COPY, TREAD);
    }
    for (i = 0; i < g_npiece; i++) /* the images to TRANSFER_DST, next to the barrier above: the GPU is idle already */
        if (!i || g_piece[i].surf != g_piece[i - 1].surf)
            vk_surf_use(cb, g_piece[i].surf, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                        VK_ACCESS_2_TRANSFER_WRITE_BIT);
    for (i = 0; i < g_npiece; i++) {
        VkBufferImageCopy copy = {.bufferOffset = g_bc_batch ? g_piece[i].boff : g_piece[i].aoff,
                                  .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, g_piece[i].level, g_piece[i].layer, 1},
                                  .imageOffset = {0, g_piece[i].y, g_piece[i].z},
                                  .imageExtent = {g_piece[i].w, g_piece[i].height, 1}};
        vkCmdCopyBufferToImage(cb, g_bc_batch ? g_bc_scratch : g_astc_scratch, g_piece[i].surf->img,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    }
    g_npiece = 0;
}
void vk_astc_upload(VkCommandBuffer cb, Surf* t, const u8* blocks, u32 w, u32 h, u32 depth, u32 level, u32 layer) {
    g_vk_astc_px += (u64)w * h * depth;
    if (depth == 1) astc_dump(blocks, w, h, t->f.bw, t->f.bh, vk_srgb_as(t->f.vk, 1) == t->f.vk);
    astc_init();
    int bc = t->f.vk != VK_FORMAT_R8G8B8A8_UNORM && t->f.vk != VK_FORMAT_R8G8B8A8_SRGB;
    u32 bw = t->f.bw, bh = t->f.bh;
    u32 nx = (w + bw - 1) / bw, ny = (h + bh - 1) / bh;
    u64 row_in = (u64)nx * 16, row_out = (u64)w * bh * 4;
    u32 rows = (u32)(g_astc_cap / row_out);
    u32 in_rows = (u32)(g_vk.props.limits.maxStorageBufferRange / row_in);
    if (rows > in_rows) rows = in_rows;
    if (rows > g_vk.props.limits.maxComputeWorkGroupCount[1]) rows = g_vk.props.limits.maxComputeWorkGroupCount[1];
    if (bc) rows &= ~3u; /* a BC3 copy starts on a whole block */
    if (!rows || (nx + 31) / 32 > g_vk.props.limits.maxComputeWorkGroupCount[0])
        fatal("vulkan: ASTC upload exceeds device limits");
    if (g_npiece && bc != (int)g_bc_batch) vk_astc_flush(cb); /* a batch is all BC3 or all RGBA8 */
    for (u32 z = 0; z < depth; z++) {
        for (u32 y = 0; y < ny; y += rows) {
            u32 n = ny - y < rows ? ny - y : rows;
            u32 height = h - y * bh < n * bh ? h - y * bh : n * bh;
            u32 bx = (w + 3) / 4, by = (height + 3) / 4;
            u64 size = row_in * n, offset, asize = align_up((u64)w * height * 4), bsize = align_up((u64)bx * by * 16);
            if (g_npiece == BATCH) vk_astc_flush(cb);
            if (g_apos + asize > g_astc_cap || (bc && g_bpos + bsize > g_astc_cap)) { /* a ring wraps */
                vk_astc_flush(cb);
                g_apos = g_bpos = 0;
                ring_wait(cb);
            } else if (vk_submit_number() != g_ring_sub) /* a new command buffer */
                ring_wait(cb);
            if (!g_npiece) g_bc_batch = (u32)bc;
            VkBuffer input;
            u8* mapped = vk_stage(size, vk_stage_align(), &input, &offset);
            if (!mapped) fatal("vulkan: cannot stage ASTC blocks");
            memcpy(mapped, blocks + ((u64)z * ny + y) * row_in, size);
            VkDescriptorSet set = vk_desc_alloc(g_astc_ds);
            if (!set) fatal("vulkan: cannot allocate ASTC descriptors");
            /* Submission makes the flushed host staging writes visible. */
            set_write(set, input, offset, size, g_astc_scratch, g_apos, (u64)w * height * 4);
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_astc_pipe);
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_astc_layout, 0, 1, &set, 0, NULL);
            AstcParams p = {w, height, bw, bh, vk_srgb_as(t->f.vk, 1) == t->f.vk};
            vkCmdPushConstants(cb, g_astc_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof p, &p);
            vkCmdDispatch(cb, (nx + 31) / 32, n, 1);
            g_piece[g_npiece] = (typeof(g_piece[0])){.surf = t,
                                                     .level = level,
                                                     .layer = layer,
                                                     .w = w,
                                                     .height = height,
                                                     .bx = bx,
                                                     .by = by,
                                                     .y = (i32)(y * bh),
                                                     .z = (i32)z,
                                                     .aoff = g_apos,
                                                     .boff = g_bpos,
                                                     .p = p};
            g_npiece++;
            t->astc_pending = 1;
            g_apos += asize;
            if (bc) g_bpos += bsize;
        }
    }
}
