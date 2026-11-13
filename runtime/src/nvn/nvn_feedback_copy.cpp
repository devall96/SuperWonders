// Feedback loops: a draw that samples an image bound as a target of its own pass.
//
// Wonder's world-map character shading (and other passes: ~8 such draws a frame) samples a colour target that its pass
// draws into, after a NVN texture barrier. On the Switch the texture reads then see what was written before the
// barrier. In Vulkan an image attached and sampled in one pass is a feedback loop, undefined without
// VK_EXT_attachment_feedback_loop_layout; on the AMD Windows driver the reads came back as stale compressed tiles:
// dark blocky squares over the characters on the world map. The TotK PC recomp found the same symptom on an RX 9070
// ("dark 16x16 blocks", refs/totk-pc-recomp docs/maxwell.md row 1.9 and check 10, docs/agents/symptoms.md) and fixed
// it with the feedback layout or a pass restart; Savage Wind does the same (runtime/nvn/vulkan/device.c).
//
// Here such a draw ends the open pass, copies each of those images (every level and layer) to a copy of its own, and
// samples the copy, while the pass, restarted, renders into the image itself: no image is attached and sampled at
// once, on any driver. The copy holds what the target held before the draw, which is what the console's texture
// barrier gives. NX_FEEDBACK_COPY=0: off (the draw samples the attached image, as before).
#include <cstdlib>
#include <map>
#include <memory>
#include <vector>

#include "nvn_draw_state.h"

namespace nx::nvn {

using namespace nx::vk;

namespace {

struct Copy {
    std::shared_ptr<GpuImage> image;
    VkImage source = VK_NULL_HANDLE;
};
std::map<VkImage, Copy> g_copies;                   // by the image they copy (re-made when it changes shape)
std::map<const GpuImage*, GpuImage*> g_substitute;  // this draw's: sampled image -> its copy
uint64_t g_copied = 0;

bool make(Copy& c, const GpuImage& src) {
    c.image = std::make_shared<GpuImage>();
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = src.format;
    ci.extent = {src.width, src.height, 1};
    ci.mipLevels = src.levels;
    ci.arrayLayers = src.layers;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (src.mutable_format) ci.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    if (src.view_type == VK_IMAGE_VIEW_TYPE_CUBE || src.view_type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY)
        ci.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    if (p_vkCreateImage(vk_device(), &ci, nullptr, &c.image->image) != VK_SUCCESS) {
        c.image.reset();
        return false;
    }
    VkMemoryRequirements req{};
    p_vkGetImageMemoryRequirements(vk_device(), c.image->image, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (p_vkAllocateMemory(vk_device(), &ai, nullptr, &c.image->memory) != VK_SUCCESS) {
        p_vkDestroyImage(vk_device(), c.image->image, nullptr);
        c.image.reset();
        return false;
    }
    p_vkBindImageMemory(vk_device(), c.image->image, c.image->memory, 0);
    GpuImage& g = *c.image;
    gpu_image_memory_allocated(req.size);
    g.allocation_size = req.size;
    g.format = src.format, g.aspect = src.aspect, g.view_type = src.view_type;
    g.width = src.width, g.height = src.height, g.levels = src.levels, g.layers = src.layers, g.depth = 1;
    g.logical_width = src.logical_width, g.logical_height = src.logical_height, g.render_scale = src.render_scale;
    g.mutable_format = src.mutable_format, g.alpha_from_r = src.alpha_from_r, g.nvn_format = src.nvn_format;
    g.rendered = true, g.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    c.source = src.image;
    return true;
}

void barrier(VkCommandBuffer cb, VkImage image, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to,
             VkAccessFlags src_access, VkAccessFlags dst_access, VkPipelineStageFlags src, VkPipelineStageFlags dst) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = src_access, b.dstAccessMask = dst_access;
    b.oldLayout = from, b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
    p_vkCmdPipelineBarrier(cb, src, dst, 0, 0, nullptr, 0, nullptr, 1, &b);
}

}  // namespace

namespace replay {

bool feedback_copy_on() {
    static const bool on = [] {
        const char* e = std::getenv("NX_FEEDBACK_COPY");
        return !(e && e[0] == '0');
    }();
    return on;
}

bool feedback_copy(VkCommandBuffer cb, GpuImage& src) {
    if (!src.image || src.view_type == VK_IMAGE_VIEW_TYPE_3D || src.depth > 1) return false;
    if (g_copies.size() > 64) {  // copies of images since destroyed: let them go (freed once no submit uses them)
        for (auto& [k, old] : g_copies) retire_image(std::move(old.image));
        g_copies.clear();
    }
    Copy& c = g_copies[src.image];
    if (c.image && (c.image->width != src.width || c.image->height != src.height || c.image->levels != src.levels ||
                    c.image->layers != src.layers || c.image->format != src.format)) {
        retire_image(std::move(c.image));  // a new image under the same handle: a copy of the new shape
        c = Copy{};
    }
    if (!c.image && !make(c, src)) return false;
    // The source stays in its layout (GENERAL, as rendered images are kept); whatever wrote or read either image is
    // done before the copy, and the copy before any later read.
    const VkPipelineStageFlags all = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    const VkAccessFlags rw = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    VkImageLayout src_layout = src.layout == VK_IMAGE_LAYOUT_UNDEFINED ? VK_IMAGE_LAYOUT_GENERAL : src.layout;
    barrier(cb, src.image, src.aspect, src.layout, src_layout, rw, VK_ACCESS_TRANSFER_READ_BIT, all,
            VK_PIPELINE_STAGE_TRANSFER_BIT);
    src.layout = src_layout;
    barrier(cb, c.image->image, c.image->aspect, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, rw,
            VK_ACCESS_TRANSFER_WRITE_BIT, all, VK_PIPELINE_STAGE_TRANSFER_BIT);
    std::vector<VkImageCopy> regions;
    for (uint32_t level = 0; level < src.levels; level++) {
        VkImageCopy r{};
        r.srcSubresource = {src.aspect, level, 0, src.layers};
        r.dstSubresource = r.srcSubresource;
        r.extent = {std::max(1u, src.width >> level), std::max(1u, src.height >> level), 1};
        regions.push_back(r);
    }
    p_vkCmdCopyImage(cb, src.image, src.layout, c.image->image, VK_IMAGE_LAYOUT_GENERAL, uint32_t(regions.size()),
                     regions.data());
    barrier(cb, c.image->image, c.image->aspect, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, all);
    c.image->layout = VK_IMAGE_LAYOUT_GENERAL;
    g_substitute[&src] = c.image.get();
    if (g_copied++ == 0)
        log("nvn/vk: feedback loop: a draw samples its own %ux%u target: sampling a copy (NX_FEEDBACK_COPY=0: off)",
            src.width, src.height);
    return true;
}

}  // namespace replay

GpuImage* feedback_substitute(const GpuImage* img) {
    if (g_substitute.empty() || !img) return nullptr;
    auto it = g_substitute.find(img);
    return it == g_substitute.end() ? nullptr : it->second;
}

namespace replay {

void feedback_clear() { g_substitute.clear(); }

uint64_t feedback_copies() { return g_copied; }

}  // namespace replay

}  // namespace nx::nvn
