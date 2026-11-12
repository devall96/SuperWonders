// AMD FidelityFX Super Resolution 1 for the presented image (third_party/fsr1, MIT): when the game's final image is
// smaller than the window (an internal resolution below the window's, or a 1080p game in a 1440p/4K window), EASU
// upscales it to the window's size and RCAS sharpens it, as two compute passes into images of their own; the result
// is copied 1:1 into the swapchain image. Otherwise, and when off, the present blits as before (bilinear).
//
// Off with NX_FSR=0 (NX_FSR=1 forces it on); else the player's setting (the overlay menu, set_fsr).
// NX_DUMP_FSR=1: frame dumps (NX_DUMP_FRAMES) write the upscaled image instead of the game's.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>

#include "nvn_vk.h"
#include "fsr_spv.h"

// The FSR headers' CPU side: FsrEasuCon / FsrRcasCon, the constants the shaders take.
#define A_CPU 1
#include "ffx_a.h"
#include "ffx_fsr1.h"

namespace nx::nvn {

using namespace nx::vk;

namespace {

std::atomic<bool> g_on{true};
std::atomic<float> g_sharpness{0.25f};  // RCAS stops: 0 is the sharpest

struct Pass {
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
};
Pass g_easu, g_rcas;
VkSampler g_sampler = VK_NULL_HANDLE;
VkDescriptorPool g_pool = VK_NULL_HANDLE;
GpuImage g_mid, g_out;  // EASU's output, RCAS's output (the window's size, RGBA8)
VkExtent2D g_extent{};
// EASU's sets, a ring rewritten one per present with that frame's source: a set is reused only 16 presents later,
// long after the GPU finished with it (frames in flight are far fewer), so no set is updated while in use.
constexpr uint32_t kRing = 16;
VkDescriptorSet g_easu_sets[kRing] = {};
uint32_t g_easu_next = 0;
VkDescriptorSet g_rcas_set = VK_NULL_HANDLE;
// UNORM views of sRGB sources (FSR works on the stored values), by image and its own view (a freed image's handle may
// come back for another).
std::map<std::pair<VkImage, VkImageView>, VkImageView> g_unorm_views;
bool g_failed = false;

int forced() {
    static const int f = [] {
        const char* e = std::getenv("NX_FSR");
        return e ? (e[0] == '0' ? 0 : 1) : -1;
    }();
    return f;
}

bool make_pass(Pass& p, const uint32_t* code, size_t bytes, uint32_t push_bytes) {
    VkDevice d = vk_device();
    VkDescriptorSetLayoutBinding b[2]{};
    b[0] = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    b[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 2;
    li.pBindings = b;
    if (p_vkCreateDescriptorSetLayout(d, &li, nullptr, &p.set_layout) != VK_SUCCESS) return false;
    VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, push_bytes};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &p.set_layout;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &range;
    if (p_vkCreatePipelineLayout(d, &pl, nullptr, &p.layout) != VK_SUCCESS) return false;
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = bytes;
    mi.pCode = code;
    VkShaderModule module = VK_NULL_HANDLE;
    if (p_vkCreateShaderModule(d, &mi, nullptr, &module) != VK_SUCCESS) return false;
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = module;
    ci.stage.pName = "main";
    ci.layout = p.layout;
    VkResult r = p_vkCreateComputePipelines(d, VK_NULL_HANDLE, 1, &ci, nullptr, &p.pipeline);
    p_vkDestroyShaderModule(d, module, nullptr);
    return r == VK_SUCCESS;
}

bool init() {
    if (g_easu.pipeline) return true;
    if (g_failed) return false;
    VkDevice d = vk_device();
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = 0;
    VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 64}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 64}};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.maxSets = 64;
    pi.poolSizeCount = 2;
    pi.pPoolSizes = sizes;
    if (p_vkCreateSampler(d, &si, nullptr, &g_sampler) != VK_SUCCESS ||
        p_vkCreateDescriptorPool(d, &pi, nullptr, &g_pool) != VK_SUCCESS ||
        !make_pass(g_easu, k_fsr_easu_comp, sizeof k_fsr_easu_comp, 64) ||
        !make_pass(g_rcas, k_fsr_rcas_comp, sizeof k_fsr_rcas_comp, 16)) {
        log("nvn/vk: FSR 1 unavailable (pipeline creation failed)");
        g_failed = true;
        return false;
    }
    log("nvn/vk: FSR 1 ready (EASU + RCAS)");
    return true;
}

bool make_image(GpuImage& img, VkExtent2D e) {
    VkDevice d = vk_device();
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = VK_FORMAT_R8G8B8A8_UNORM;
    ci.extent = {e.width, e.height, 1};
    ci.mipLevels = ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (p_vkCreateImage(d, &ci, nullptr, &img.image) != VK_SUCCESS) return false;
    VkMemoryRequirements req{};
    p_vkGetImageMemoryRequirements(d, img.image, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (p_vkAllocateMemory(d, &ai, nullptr, &img.memory) != VK_SUCCESS) return false;
    p_vkBindImageMemory(d, img.image, img.memory, 0);
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = img.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = ci.format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (p_vkCreateImageView(d, &vi, nullptr, &img.view) != VK_SUCCESS) return false;
    img.format = ci.format;
    img.width = e.width, img.height = e.height;
    img.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    return true;
}

void drop_image(GpuImage& img) {
    VkDevice d = vk_device();
    if (img.view) p_vkDestroyImageView(d, img.view, nullptr);
    if (img.image) p_vkDestroyImage(d, img.image, nullptr);
    if (img.memory) p_vkFreeMemory(d, img.memory, nullptr);
    img = GpuImage{};
}

void write_set(VkDescriptorSet set, VkImageView sampled, VkImageLayout sampled_layout, VkImageView storage);

VkDescriptorSet make_set(const Pass& p, VkImageView sampled, VkImageLayout sampled_layout, VkImageView storage) {
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = g_pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &p.set_layout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (p_vkAllocateDescriptorSets(vk_device(), &ai, &set) != VK_SUCCESS) return VK_NULL_HANDLE;
    write_set(set, sampled, sampled_layout, storage);
    return set;
}

void write_set(VkDescriptorSet set, VkImageView sampled, VkImageLayout sampled_layout, VkImageView storage) {
    VkDescriptorImageInfo in{g_sampler, sampled, sampled_layout}, out{VK_NULL_HANDLE, storage, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w[2]{};
    w[0].sType = w[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w[0].dstSet = w[1].dstSet = set;
    w[0].dstBinding = 0, w[0].descriptorCount = 1, w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w[0].pImageInfo = &in;
    w[1].dstBinding = 1, w[1].descriptorCount = 1, w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    w[1].pImageInfo = &out;
    p_vkUpdateDescriptorSets(vk_device(), 2, w, 0, nullptr);
}

// The images and sets for a window of `e` pixels (re-made when it changes; the GPU is idle first).
bool ensure_size(VkExtent2D e) {
    if (g_mid.image && e.width == g_extent.width && e.height == g_extent.height) return true;
    p_vkDeviceWaitIdle(vk_device());
    drop_image(g_mid), drop_image(g_out);
    p_vkResetDescriptorPool(vk_device(), g_pool, 0);
    for (VkDescriptorSet& set : g_easu_sets) set = VK_NULL_HANDLE;
    g_rcas_set = VK_NULL_HANDLE;
    if (!make_image(g_mid, e) || !make_image(g_out, e)) {
        log("nvn/vk: FSR 1 images for %ux%u failed", e.width, e.height);
        drop_image(g_mid), drop_image(g_out);
        return false;
    }
    g_rcas_set = make_set(g_rcas, g_mid.view, VK_IMAGE_LAYOUT_GENERAL, g_out.view);
    for (VkDescriptorSet& set : g_easu_sets)
        if (!(set = make_set(g_easu, g_mid.view, VK_IMAGE_LAYOUT_GENERAL, g_mid.view))) return false;  // filled per frame
    g_extent = e;
    log("nvn/vk: FSR 1 output %ux%u", e.width, e.height);
    return g_rcas_set != VK_NULL_HANDLE;
}

// A view FSR can read the source's stored (perceptual) values through: its own for UNORM formats, a UNORM view of
// an sRGB one. Null when there is none (an image not made MUTABLE_FORMAT, an HDR format).
VkImageView source_view(GpuImage& src) {
    switch (src.format) {
        case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_B8G8R8A8_UNORM:
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return src.view;
        case VK_FORMAT_R8G8B8A8_SRGB:
        case VK_FORMAT_B8G8R8A8_SRGB: break;
        default: return VK_NULL_HANDLE;
    }
    if (!src.mutable_format) return VK_NULL_HANDLE;
    VkImageView& v = g_unorm_views[{src.image, src.view}];
    if (!v) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = src.image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = src.format == VK_FORMAT_R8G8B8A8_SRGB ? VK_FORMAT_R8G8B8A8_UNORM : VK_FORMAT_B8G8R8A8_UNORM;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (p_vkCreateImageView(vk_device(), &vi, nullptr, &v) != VK_SUCCESS) v = VK_NULL_HANDLE;
    }
    return v;
}

}  // namespace

void set_fsr(bool on, float sharpness) {
    g_on = on;
    g_sharpness = std::clamp(sharpness, 0.0f, 2.0f);
}

bool fsr_active() { return forced() >= 0 ? forced() == 1 : g_on.load(); }

bool fsr_record(VkCommandBuffer cb, GpuImage& src, GpuImage& dst, VkExtent2D extent, const int32_t* crop) {
    if (!fsr_active() || !extent.width || !extent.height) return false;
    // Upscaling only, of the whole image (a crop that is not all of it keeps the blit).
    if (src.width > extent.width || src.height > extent.height || (src.width == extent.width && src.height == extent.height))
        return false;
    const uint32_t lw = src.logical_width ? src.logical_width : src.width;
    const uint32_t lh = src.logical_height ? src.logical_height : src.height;
    if (crop && crop[2] > 0 && crop[3] > 0 && (crop[0] || crop[1] || uint32_t(crop[2]) != lw || uint32_t(crop[3]) != lh))
        return false;
    if (!init() || !ensure_size(extent)) return false;
    VkImageView view = source_view(src);
    if (!view) return false;
    VkDescriptorSet easu_set = g_easu_sets[g_easu_next++ % kRing];
    write_set(easu_set, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, g_mid.view);

    transition(cb, src, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    transition(cb, g_mid, VK_IMAGE_LAYOUT_GENERAL);
    transition(cb, g_out, VK_IMAGE_LAYOUT_GENERAL);
    const uint32_t gx = (extent.width + 15) / 16, gy = (extent.height + 15) / 16;

    AU1 easu[16] = {};
    FsrEasuCon(easu, easu + 4, easu + 8, easu + 12, AF1(src.width), AF1(src.height), AF1(src.width), AF1(src.height),
               AF1(extent.width), AF1(extent.height));
    p_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_easu.pipeline);
    p_vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_easu.layout, 0, 1, &easu_set, 0, nullptr);
    p_vkCmdPushConstants(cb, g_easu.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof easu, easu);
    p_vkCmdDispatch(cb, gx, gy, 1);

    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};  // EASU's writes before RCAS reads them
    b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    b.oldLayout = b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = g_mid.image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                           0, nullptr, 1, &b);

    AU1 rcas[4] = {};
    FsrRcasCon(rcas, g_sharpness.load());
    p_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_rcas.pipeline);
    p_vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_rcas.layout, 0, 1, &g_rcas_set, 0, nullptr);
    p_vkCmdPushConstants(cb, g_rcas.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof rcas, rcas);
    p_vkCmdDispatch(cb, gx, gy, 1);

    transition(cb, g_out, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    transition(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageBlit blit{};
    blit.srcSubresource = blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[1] = blit.dstOffsets[1] = {int32_t(extent.width), int32_t(extent.height), 1};
    p_vkCmdBlitImage(cb, g_out.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     1, &blit, VK_FILTER_NEAREST);
    static bool logged = false;
    if (!logged) {
        logged = true;
        log("nvn/vk: FSR 1 upscaling %ux%u (Vulkan format %d) to %ux%u (sharpness %.2f stops)", src.width, src.height,
            int(src.format), extent.width, extent.height, double(g_sharpness.load()));
    }
    return true;
}

GpuImage* fsr_output() {
    static const bool dump = std::getenv("NX_DUMP_FSR") != nullptr;
    return dump && g_out.image ? &g_out : nullptr;
}

void fsr_release() {
    if (!g_mid.image) return;
    p_vkDeviceWaitIdle(vk_device());
    drop_image(g_mid), drop_image(g_out);
    if (g_pool) p_vkResetDescriptorPool(vk_device(), g_pool, 0);
    for (VkDescriptorSet& set : g_easu_sets) set = VK_NULL_HANDLE;
    g_rcas_set = VK_NULL_HANDLE, g_extent = {};
}

}  // namespace nx::nvn
