// The GPU ASTC decoder (runtime/shaders/astc_decode.comp) against the CPU one (runtime/src/nvn/nvn_astc.cpp): random
// blocks of every block size, UNORM and sRGB, on images that end inside a block, decoded by both; every texel must
// match. Needs a Vulkan device (exits 0 with a note without one). Built by CMake with -DNX_TESTS=ON.
#define _CRT_SECURE_NO_WARNINGS
#include <vulkan/vulkan.h>

#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

#include "nvn_astc.h"

namespace {

const uint32_t kSpirv[] = {
#include "astc_decode.comp.inc"
};

struct Push {
    uint32_t width, height, slices, bw, bh, srgb, in_word, out_word;
};

#define CHECK(x)                                                    \
    do {                                                            \
        if ((x) != VK_SUCCESS) {                                    \
            std::printf("FAIL: %s\n", #x);                          \
            return 1;                                               \
        }                                                           \
    } while (0)

struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* ptr = nullptr;
};

bool make_buffer(VkPhysicalDevice phys, VkDevice d, VkDeviceSize size, Buffer& b) {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size, bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (vkCreateBuffer(d, &bi, nullptr, &b.buffer) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(d, b.buffer, &req);
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size, ai.memoryTypeIndex = UINT32_MAX;
    for (uint32_t k = 0; k < mp.memoryTypeCount && ai.memoryTypeIndex == UINT32_MAX; k++)
        if ((req.memoryTypeBits >> k & 1) && (mp.memoryTypes[k].propertyFlags & want) == want) ai.memoryTypeIndex = k;
    return ai.memoryTypeIndex != UINT32_MAX && vkAllocateMemory(d, &ai, nullptr, &b.memory) == VK_SUCCESS &&
           vkBindBufferMemory(d, b.buffer, b.memory, 0) == VK_SUCCESS &&
           vkMapMemory(d, b.memory, 0, VK_WHOLE_SIZE, 0, &b.ptr) == VK_SUCCESS;
}

}  // namespace

int main() {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_0;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    VkInstance inst;
    uint32_t n = 0;
    if (vkCreateInstance(&ici, nullptr, &inst) != VK_SUCCESS || vkEnumeratePhysicalDevices(inst, &n, nullptr) || !n) {
        std::printf("no Vulkan device: skipped\n");
        return 0;
    }
    std::vector<VkPhysicalDevice> devs(n);
    vkEnumeratePhysicalDevices(inst, &n, devs.data());
    VkPhysicalDevice phys = devs[0];
    uint32_t qn = 0, family = UINT32_MAX;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qf(qn);
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &qn, qf.data());
    for (uint32_t k = 0; k < qn && family == UINT32_MAX; k++)
        if (qf[k].queueFlags & VK_QUEUE_COMPUTE_BIT) family = k;
    const float prio = 1;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = family, qci.queueCount = 1, qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1, dci.pQueueCreateInfos = &qci;
    VkDevice d;
    CHECK(vkCreateDevice(phys, &dci, nullptr, &d));
    VkQueue queue;
    vkGetDeviceQueue(d, family, 0, &queue);

    VkDescriptorSetLayoutBinding b[2] = {{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                                         {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo sci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    sci.bindingCount = 2, sci.pBindings = b;
    VkDescriptorSetLayout set_layout;
    CHECK(vkCreateDescriptorSetLayout(d, &sci, nullptr, &set_layout));
    VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push)};
    VkPipelineLayoutCreateInfo lci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    lci.setLayoutCount = 1, lci.pSetLayouts = &set_layout, lci.pushConstantRangeCount = 1, lci.pPushConstantRanges = &push;
    VkPipelineLayout layout;
    CHECK(vkCreatePipelineLayout(d, &lci, nullptr, &layout));
    VkShaderModuleCreateInfo mci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mci.codeSize = sizeof kSpirv, mci.pCode = kSpirv;
    VkShaderModule module;
    CHECK(vkCreateShaderModule(d, &mci, nullptr, &module));
    VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpi.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, module, "main"};
    cpi.layout = layout;
    VkPipeline pipeline;
    CHECK(vkCreateComputePipelines(d, VK_NULL_HANDLE, 1, &cpi, nullptr, &pipeline));

    // 96 x 96 blocks per size; the image ends one texel inside the last block column and row.
    constexpr uint32_t kBlocks = 96;
    constexpr uint64_t kMaxTexels = uint64_t(kBlocks * 12) * (kBlocks * 12);
    Buffer in, out;
    if (!make_buffer(phys, d, kBlocks * kBlocks * 16 + 256, in) || !make_buffer(phys, d, kMaxTexels * 4 + 256, out)) {
        std::printf("FAIL: buffers\n");
        return 1;
    }
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2};
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets = 1, pci.poolSizeCount = 1, pci.pPoolSizes = &ps;
    VkDescriptorPool pool;
    CHECK(vkCreateDescriptorPool(d, &pci, nullptr, &pool));
    VkDescriptorSetAllocateInfo sai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    sai.descriptorPool = pool, sai.descriptorSetCount = 1, sai.pSetLayouts = &set_layout;
    VkDescriptorSet set;
    CHECK(vkAllocateDescriptorSets(d, &sai, &set));
    VkDescriptorBufferInfo bi[2] = {{in.buffer, 0, VK_WHOLE_SIZE}, {out.buffer, 0, VK_WHOLE_SIZE}};
    VkWriteDescriptorSet w[2] = {};
    for (uint32_t k = 0; k < 2; k++) {
        w[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[k].dstSet = set, w[k].dstBinding = k, w[k].descriptorCount = 1;
        w[k].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, w[k].pBufferInfo = &bi[k];
    }
    vkUpdateDescriptorSets(d, 2, w, 0, nullptr);
    VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, cpci.queueFamilyIndex = family;
    VkCommandPool cmd_pool;
    CHECK(vkCreateCommandPool(d, &cpci, nullptr, &cmd_pool));
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = cmd_pool, cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, cai.commandBufferCount = 1;
    VkCommandBuffer cb;
    CHECK(vkAllocateCommandBuffers(d, &cai, &cb));

    const uint32_t sizes[14][2] = {{4, 4}, {5, 4},  {5, 5},  {6, 5},  {6, 6},   {8, 5},   {8, 6},
                                   {8, 8}, {10, 5}, {10, 6}, {10, 8}, {10, 10}, {12, 10}, {12, 12}};
    std::mt19937 rng(1234);
    int failed = 0;
    uint64_t texels = 0, valid = 0;
    for (const auto& s : sizes)
        for (uint32_t srgb = 0; srgb < 2; srgb++) {
            const uint32_t bw = s[0], bh = s[1], width = kBlocks * bw - 1, height = kBlocks * bh - 1;
            // Out of the in buffer at word 4 and the out buffer at word 8: the offsets the push constants carry.
            uint8_t* blocks = static_cast<uint8_t*>(in.ptr) + 16;
            // A quarter random (mostly invalid), a quarter void extents (LDR and HDR), half random valid encodings.
            uint8_t scratch[12 * 12 * 4];
            for (uint32_t k = 0; k < kBlocks * kBlocks; k++) {
                uint8_t* blk = blocks + k * 16;
                do {
                    for (int j = 0; j < 16; j++) blk[j] = uint8_t(rng());
                    if (k % 4 == 1) blk[0] = 0xFC, blk[1] = uint8_t(0xFC | (rng() & 2));
                } while (k % 4 >= 2 && !nx::nvn::astc_decode_block(blk, bw, bh, srgb != 0, scratch));
            }
            std::vector<uint8_t> cpu(uint64_t(width) * height * 4);
            uint64_t bad = nx::nvn::astc_decode_image(blocks, width, height, 1, bw, bh, srgb != 0, cpu.data());
            valid += uint64_t(kBlocks) * kBlocks - bad;
            VkCommandBufferBeginInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            CHECK(vkBeginCommandBuffer(cb, &cbi));
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
            Push pc{width, height, 1, bw, bh, srgb, 4, 8};
            vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
            vkCmdDispatch(cb, (width + 7) / 8, (height + 7) / 8, 1);
            VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0,
                                 nullptr, 0, nullptr);
            CHECK(vkEndCommandBuffer(cb));
            VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            si.commandBufferCount = 1, si.pCommandBuffers = &cb;
            CHECK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
            CHECK(vkQueueWaitIdle(queue));
            const uint8_t* gpu = static_cast<const uint8_t*>(out.ptr) + 32;
            uint64_t diff = 0;
            for (uint64_t k = 0; k < cpu.size(); k++)
                if (cpu[k] != gpu[k] && !diff++)
                    std::printf("FAIL: %ux%u srgb %u: texel (%llu, %llu) channel %llu: CPU %u, GPU %u\n", bw, bh,
                                srgb, (unsigned long long)(k / 4 % width), (unsigned long long)(k / 4 / width),
                                (unsigned long long)(k % 4), cpu[k], gpu[k]);
            failed += diff != 0;
            texels += cpu.size() / 4;
        }
    std::printf("%s: %llu texels, %llu valid blocks of %llu\n", failed ? "FAILED" : "ok", (unsigned long long)texels,
                (unsigned long long)valid, (unsigned long long)(28ull * kBlocks * kBlocks));
    vkDestroyDevice(d, nullptr);
    vkDestroyInstance(inst, nullptr);
    return failed ? 1 : 0;
}
