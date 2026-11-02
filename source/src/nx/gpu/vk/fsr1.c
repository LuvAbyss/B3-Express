/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* AMD FSR 1: EASU upscales, RCAS sharpens. The constants are FsrEasuCon's and FsrRcasCon's (shaders/ffx/ffx_fsr1.h, AMD,
   MIT) */
#include <math.h>
#include <string.h>
#include "nx/gpu/vk/fsr1.h"
#include "nx/gpu/vk/fsr1_spv.h"
#include "nx/runtime/platform/log.h"

typedef struct {
    u32 con[16];
    i32 size[2];
} Fsr1Push;

static VkDescriptorSetLayout g_ds;
static VkPipelineLayout g_layout;
static VkPipeline g_easu, g_rcas;
static VkSampler g_sampler;

int vk_fsr1_ready(void) { return FSR1_SPV_READY; }

static u32 f2u(float v) {
    u32 u;
    memcpy(&u, &v, 4);
    return u;
}

static VkPipeline make_pipeline(const u32* code, size_t size) {
    VkShaderModuleCreateInfo mi = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = size, .pCode = code};
    VkShaderModule module;
    VkPipeline pipe = VK_NULL_HANDLE;
    if (!VK_OK(vkCreateShaderModule(g_vk.dev, &mi, NULL, &module))) return VK_NULL_HANDLE;
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                                .module = module,
                                                .pName = "main"},
                                      .layout = g_layout};
    VK_OK(vkCreateComputePipelines(g_vk.dev, vk_pipeline_cache(), 1, &ci, NULL, &pipe));
    vkDestroyShaderModule(g_vk.dev, module, NULL);
    return pipe;
}

static int fsr1_init(void) {
    static int tried;
    if (tried) return g_easu && g_rcas;
    tried = 1;
    VkDescriptorSetLayoutBinding b[2] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
    VkDescriptorSetLayoutCreateInfo di = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = b};
    VkPushConstantRange push = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Fsr1Push)};
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                     .setLayoutCount = 1,
                                     .pSetLayouts = &g_ds,
                                     .pushConstantRangeCount = 1,
                                     .pPushConstantRanges = &push};
    VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                              .magFilter = VK_FILTER_LINEAR,
                              .minFilter = VK_FILTER_LINEAR,
                              .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    if (!VK_OK(vkCreateDescriptorSetLayout(g_vk.dev, &di, NULL, &g_ds)) ||
        !VK_OK(vkCreatePipelineLayout(g_vk.dev, &li, NULL, &g_layout)) ||
        !VK_OK(vkCreateSampler(g_vk.dev, &si, NULL, &g_sampler)))
        return 0;
    g_easu = make_pipeline(fsr1_easu_spv, sizeof fsr1_easu_spv);
    g_rcas = make_pipeline(fsr1_rcas_spv, sizeof fsr1_rcas_spv);
    if (!g_easu || !g_rcas) WARN("vulkan: the FSR 1 pipelines could not be made\n");
    return g_easu && g_rcas;
}

static void pass(VkCommandBuffer cb, VkPipeline pipe, VkImageView in, VkImageView out, const Fsr1Push* p) {
    VkDescriptorSet set = vk_desc_alloc(g_ds);
    VkDescriptorImageInfo ii[2] = {{g_sampler, in, VK_IMAGE_LAYOUT_GENERAL},
                                   {VK_NULL_HANDLE, out, VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet w[2] = {{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                  .dstSet = set,
                                  .dstBinding = 0,
                                  .descriptorCount = 1,
                                  .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                  .pImageInfo = &ii[0]},
                                 {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                  .dstSet = set,
                                  .dstBinding = 1,
                                  .descriptorCount = 1,
                                  .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                  .pImageInfo = &ii[1]}};
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                           .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                           .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                           .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT};
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    if (!set) return;
    vkUpdateDescriptorSets(g_vk.dev, 2, w, 0, NULL);
    vkCmdPipelineBarrier2(cb, &d);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_layout, 0, 1, &set, 0, NULL);
    vkCmdPushConstants(cb, g_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof *p, p);
    vkCmdDispatch(cb, ((u32)p->size[0] + 15) / 16, ((u32)p->size[1] + 15) / 16, 1);
}

void vk_fsr1_run(VkCommandBuffer cb, VkImageView src, u32 sw, u32 sh, VkImageView tmp, VkImageView dst, u32 dw, u32 dh,
                 float sharpness) {
    float x = (float)sw, y = (float)sh, ox = (float)dw, oy = (float)dh;
    Fsr1Push p;
    if (!fsr1_init()) return;
    memset(&p, 0, sizeof p);
    p.size[0] = (i32)dw;
    p.size[1] = (i32)dh;
    p.con[0] = f2u(x / ox);
    p.con[1] = f2u(y / oy);
    p.con[2] = f2u(0.5f * x / ox - 0.5f);
    p.con[3] = f2u(0.5f * y / oy - 0.5f);
    p.con[4] = f2u(1.0f / x);
    p.con[5] = f2u(1.0f / y);
    p.con[6] = f2u(1.0f / x);
    p.con[7] = f2u(-1.0f / y);
    p.con[8] = f2u(-1.0f / x);
    p.con[9] = f2u(2.0f / y);
    p.con[10] = f2u(1.0f / x);
    p.con[11] = f2u(2.0f / y);
    p.con[12] = f2u(0.0f);
    p.con[13] = f2u(4.0f / y);
    pass(cb, g_easu, src, tmp, &p);
    memset(p.con, 0, sizeof p.con);
    p.con[0] = f2u(powf(2.0f, -sharpness));
    pass(cb, g_rcas, tmp, dst, &p);
}
