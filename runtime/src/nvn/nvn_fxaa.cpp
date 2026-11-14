// FXAA on the game's final image before it is presented (runtime/shaders/post/fxaa.comp): one compute pass into an
// RGBA8 image of the image's own size, which the present then upscales (FSR 1, nvn_fsr.cpp) or blits instead of the
// game's image. Off by default (the player's setting, the overlay menu's Anti-aliasing); NX_FXAA=1 forces it on,
// NX_FXAA=0 off.
#include <atomic>
#include <cstdlib>

#include "nvn_vk.h"
#include "post_spv.h"

namespace nx::nvn {

using namespace nx::vk;

namespace {

std::atomic<bool> g_on{false};
VkDescriptorSetLayout g_set_layout = VK_NULL_HANDLE;
VkPipelineLayout g_layout = VK_NULL_HANDLE;
VkPipeline g_pipeline = VK_NULL_HANDLE;
VkSampler g_sampler = VK_NULL_HANDLE;
VkDescriptorPool g_pool = VK_NULL_HANDLE;
constexpr uint32_t kRing = 16;  // sets rewritten one per present (as nvn_fsr.cpp's)
VkDescriptorSet g_sets[kRing] = {};
uint32_t g_next = 0;
GpuImage g_out;
bool g_failed = false;

int forced() {
    static const int f = [] {
        const char* e = std::getenv("NX_FXAA");
        return e ? (e[0] == '0' ? 0 : 1) : -1;
    }();
    return f;
}

bool init() {
    if (g_pipeline) return true;
    if (g_failed) return false;
    VkDevice d = vk_device();
    VkDescriptorSetLayoutBinding b[2]{};
    b[0] = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    b[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 2;
    li.pBindings = b;
    VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, 8};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &g_set_layout;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &range;
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = sizeof k_fxaa_comp;
    mi.pCode = k_fxaa_comp;
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kRing},
                                     {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kRing}};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.maxSets = kRing;
    pi.poolSizeCount = 2;
    pi.pPoolSizes = sizes;
    VkShaderModule module = VK_NULL_HANDLE;
    bool ok = p_vkCreateDescriptorSetLayout(d, &li, nullptr, &g_set_layout) == VK_SUCCESS &&
              p_vkCreatePipelineLayout(d, &pl, nullptr, &g_layout) == VK_SUCCESS &&
              p_vkCreateShaderModule(d, &mi, nullptr, &module) == VK_SUCCESS &&
              p_vkCreateSampler(d, &si, nullptr, &g_sampler) == VK_SUCCESS &&
              p_vkCreateDescriptorPool(d, &pi, nullptr, &g_pool) == VK_SUCCESS;
    if (ok) {
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        ci.stage.module = module;
        ci.stage.pName = "main";
        ci.layout = g_layout;
        ok = p_vkCreateComputePipelines(d, VK_NULL_HANDLE, 1, &ci, nullptr, &g_pipeline) == VK_SUCCESS;
        VkDescriptorSetLayout layouts[kRing];
        for (VkDescriptorSetLayout& l : layouts) l = g_set_layout;
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = g_pool;
        ai.descriptorSetCount = kRing;
        ai.pSetLayouts = layouts;
        ok = ok && p_vkAllocateDescriptorSets(d, &ai, g_sets) == VK_SUCCESS;
    }
    if (module) p_vkDestroyShaderModule(d, module, nullptr);
    if (!ok) {
        log("nvn/vk: FXAA unavailable (pipeline creation failed)");
        g_failed = true;
        return false;
    }
    log("nvn/vk: FXAA ready");
    return true;
}

void drop_output() {
    VkDevice d = vk_device();
    if (g_out.view) p_vkDestroyImageView(d, g_out.view, nullptr);
    if (g_out.image) p_vkDestroyImage(d, g_out.image, nullptr);
    if (g_out.memory) p_vkFreeMemory(d, g_out.memory, nullptr);
    g_out = GpuImage{};
}

bool ensure_output(uint32_t w, uint32_t h) {
    if (g_out.image && g_out.width == w && g_out.height == h) return true;
    if (g_out.image) p_vkDeviceWaitIdle(vk_device());
    drop_output();
    VkDevice d = vk_device();
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = VK_FORMAT_R8G8B8A8_UNORM;
    ci.extent = {w, h, 1};
    ci.mipLevels = ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (p_vkCreateImage(d, &ci, nullptr, &g_out.image) != VK_SUCCESS) return false;
    VkMemoryRequirements req{};
    p_vkGetImageMemoryRequirements(d, g_out.image, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (p_vkAllocateMemory(d, &ai, nullptr, &g_out.memory) != VK_SUCCESS) return false;
    p_vkBindImageMemory(d, g_out.image, g_out.memory, 0);
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = g_out.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = ci.format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (p_vkCreateImageView(d, &vi, nullptr, &g_out.view) != VK_SUCCESS) return false;
    g_out.format = ci.format;
    g_out.width = w, g_out.height = h;
    g_out.mutable_format = false;
    log("nvn/vk: FXAA output %ux%u", w, h);
    return true;
}

}  // namespace

void set_fxaa(bool on) { g_on = on; }

GpuImage* fxaa_record(VkCommandBuffer cb, GpuImage& src) {
    if (!(forced() >= 0 ? forced() == 1 : g_on.load())) return nullptr;
    // RGBA8 sources only (the game's final image); sRGB ones are left alone rather than filtered in the wrong space.
    if (src.format != VK_FORMAT_R8G8B8A8_UNORM && src.format != VK_FORMAT_B8G8R8A8_UNORM) return nullptr;
    if (!init() || !ensure_output(src.width, src.height)) return nullptr;
    VkDescriptorSet set = g_sets[g_next++ % kRing];
    VkDescriptorImageInfo in{g_sampler, src.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo out{VK_NULL_HANDLE, g_out.view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w[2]{};
    w[0].sType = w[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w[0].dstSet = w[1].dstSet = set;
    w[0].dstBinding = 0, w[0].descriptorCount = 1, w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w[0].pImageInfo = &in;
    w[1].dstBinding = 1, w[1].descriptorCount = 1, w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    w[1].pImageInfo = &out;
    p_vkUpdateDescriptorSets(vk_device(), 2, w, 0, nullptr);

    transition(cb, src, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    transition(cb, g_out, VK_IMAGE_LAYOUT_GENERAL);
    float inv[2] = {1.0f / float(src.width), 1.0f / float(src.height)};
    p_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_pipeline);
    p_vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_layout, 0, 1, &set, 0, nullptr);
    p_vkCmdPushConstants(cb, g_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof inv, inv);
    p_vkCmdDispatch(cb, (src.width + 7) / 8, (src.height + 7) / 8, 1);
    // Read next by FSR (sampled) or the blit (transfer): the transition there orders it.
    g_out.logical_width = src.logical_width, g_out.logical_height = src.logical_height;
    static bool logged = false;
    if (!logged) logged = true, log("nvn/vk: FXAA on the presented image (%ux%u)", src.width, src.height);
    return &g_out;
}

void fxaa_release() {
    if (!g_out.image) return;
    p_vkDeviceWaitIdle(vk_device());
    drop_output();
}

}  // namespace nx::nvn
