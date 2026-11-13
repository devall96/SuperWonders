// First-run native shader preparation from the private catalog made by tools/maxwell/extract_romfs_shaders.py.
// The catalog contains real program/stage relationships found in the selected RomFS. We build real Vulkan
// compute and graphics pipelines into the driver's persistent VkPipelineCache before the first game frame.
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "nvn_pipeline.h"
#include "nvn_pipeline_async.h"
#include "nvn_shader_prepare.h"
#include "platform.h"

namespace nx::nvn {

using namespace nx::vk;

namespace {

constexpr char kMagic[8] = {'W', 'O', 'N', 'S', 'H', 'D', '0', '1'};
constexpr size_t kMaxStages = 100000;
constexpr size_t kMaxPrograms = 100000;

// program_layout() keeps its keys for the process lifetime and keys them by ShaderInfo address.
// Keep these catalog records alive after preparation so those cache keys cannot dangle or alias
// a later game's ShaderInfo allocation.
std::vector<std::unique_ptr<ShaderInfo>> g_catalog_infos;

struct StageRec {
    std::string hash;
    ShaderStage stage = ShaderStage::Unknown;
    uint32_t shared = 0;
    uint32_t local[3] = {1, 1, 1};
};
struct ProgramRec { std::vector<uint32_t> stages; };

uint32_t read_u32(const std::vector<uint8_t>& bytes, size_t& at, bool& ok) {
    if (at > bytes.size() || bytes.size() - at < 4) return ok = false, 0;
    uint32_t v;
    std::memcpy(&v, bytes.data() + at, 4);
    at += 4;
    return v;
}

bool read_catalog(std::vector<StageRec>& stages, std::vector<ProgramRec>& programs, uint64_t& fingerprint) {
    const std::string path = plat::data_root() + "/shader_programs.bin";
    FILE* f = plat::fopen_utf8(path, "rb");
    if (!f) return false;
    plat::fseek64(f, 0, SEEK_END);
    const int64_t length = plat::ftell64(f);
    plat::fseek64(f, 0, SEEK_SET);
    if (length < 16 || length > (256ll << 20)) { std::fclose(f); return false; }
    std::vector<uint8_t> bytes(static_cast<size_t>(length));
    const bool got = std::fread(bytes.data(), 1, bytes.size(), f) == bytes.size();
    std::fclose(f);
    if (!got || std::memcmp(bytes.data(), kMagic, sizeof kMagic)) return false;

    // FNV-1a is only a local cache-version key, not a security check.
    fingerprint = 0xcbf29ce484222325ull;
    for (uint8_t b : bytes) fingerprint = (fingerprint ^ b) * 0x100000001b3ull;
    size_t at = 8;
    bool ok = true;
    const uint32_t program_count = read_u32(bytes, at, ok), stage_count = read_u32(bytes, at, ok);
    if (!ok || program_count > kMaxPrograms || stage_count > kMaxStages ||
        uint64_t(stage_count) * 30 > bytes.size() - at)
        return false;
    stages.reserve(stage_count);
    for (uint32_t i = 0; i < stage_count; i++) {
        StageRec s;
        if (bytes.size() - at < 30) return false;
        static const char* hex = "0123456789abcdef";
        s.hash.resize(16);
        for (int k = 0; k < 8; k++) {
            const uint8_t b = bytes[at++];
            s.hash[2 * k] = hex[b >> 4];
            s.hash[2 * k + 1] = hex[b & 15];
        }
        const uint8_t stage = bytes[at++];
        at += 5;  // reserved, zero in the versioned catalog
        s.shared = read_u32(bytes, at, ok);
        for (uint32_t& n : s.local) n = read_u32(bytes, at, ok);
        if (!ok || stage < 1 || stage > 3 || s.shared > (64u << 20)) return false;
        s.stage = stage == 1 ? ShaderStage::Vertex : stage == 2 ? ShaderStage::Fragment : ShaderStage::Compute;
        for (uint32_t& n : s.local) if (!n) n = 1;
        stages.push_back(std::move(s));
    }
    programs.reserve(program_count);
    for (uint32_t i = 0; i < program_count; i++) {
        const uint32_t count = read_u32(bytes, at, ok);
        if (!ok || !count || count > 8 || uint64_t(count) * 4 > bytes.size() - at) return false;
        ProgramRec p;
        p.stages.reserve(count);
        for (uint32_t k = 0; k < count; k++) {
            const uint32_t index = read_u32(bytes, at, ok);
            if (!ok || index >= stage_count) return false;
            p.stages.push_back(index);
        }
        programs.push_back(std::move(p));
    }
    return at == bytes.size() && !programs.empty() && !stages.empty();
}

std::unique_ptr<ShaderInfo> load_catalog_shader(const StageRec& rec) {
    const std::string path = plat::data_root() + "/spirv/" + rec.hash + ".spv";
    FILE* f = plat::fopen_utf8(path, "rb");
    if (!f) return nullptr;
    plat::fseek64(f, 0, SEEK_END);
    const int64_t size = plat::ftell64(f);
    plat::fseek64(f, 0, SEEK_SET);
    if (size < 20 || size > (64ll << 20) || (size & 3)) { std::fclose(f); return nullptr; }
    auto info = std::make_unique<ShaderInfo>();
    info->hash = rec.hash;
    info->stage = rec.stage;
    info->control.valid = true;
    info->control.shared_size = rec.shared;
    info->control.local_size[0] = rec.local[0];
    info->control.local_size[1] = rec.local[1];
    info->control.local_size[2] = rec.local[2];
    info->local_size[0] = rec.local[0];
    info->local_size[1] = rec.local[1];
    info->local_size[2] = rec.local[2];
    info->spirv.resize(size_t(size) / 4);
    const bool got = std::fread(info->spirv.data(), 1, size_t(size), f) == size_t(size);
    std::fclose(f);
    if (!got) return nullptr;
    std::string error;
    if (!reflect_spirv(info->spirv.data(), info->spirv.size(), info->refl, &error) || info->refl.stage != rec.stage) {
        log("nvn/vk: precompile shader %s rejected by reflection%s%s", rec.hash.c_str(), error.empty() ? "" : ": ",
            error.c_str());
        return nullptr;
    }
    return info;
}

VkShaderModule make_module(const ShaderInfo* shader, uint32_t unwritten = 0) {
    std::vector<uint32_t> code = patched_spirv(shader, nullptr, unwritten, nullptr);
    if (shader->stage == ShaderStage::Vertex) clip_z_module(shader, code);
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = code.size() * sizeof(uint32_t);
    ci.pCode = code.data();
    VkShaderModule module = VK_NULL_HANDLE;
    if (p_vkCreateShaderModule(vk_device(), &ci, nullptr, &module) != VK_SUCCESS) return VK_NULL_HANDLE;
    return module;
}

VkFormat io_format(const ShaderIo& io, bool attachment) {
    const uint32_t n = std::clamp(io.components, 1u, 4u);
    if (attachment) {
        if (io.type == ScalarType::Sint) return VK_FORMAT_R8G8B8A8_SINT;
        if (io.type == ScalarType::Uint || io.type == ScalarType::Bool) return VK_FORMAT_R8G8B8A8_UINT;
        return VK_FORMAT_R8G8B8A8_UNORM;
    }
    if (io.type == ScalarType::Sint) {
        constexpr VkFormat f[] = {VK_FORMAT_UNDEFINED, VK_FORMAT_R32_SINT, VK_FORMAT_R32G32_SINT,
                                  VK_FORMAT_R32G32B32_SINT, VK_FORMAT_R32G32B32A32_SINT};
        return f[n];
    }
    if (io.type == ScalarType::Uint || io.type == ScalarType::Bool) {
        constexpr VkFormat f[] = {VK_FORMAT_UNDEFINED, VK_FORMAT_R32_UINT, VK_FORMAT_R32G32_UINT,
                                  VK_FORMAT_R32G32B32_UINT, VK_FORMAT_R32G32B32A32_UINT};
        return f[n];
    }
    constexpr VkFormat f[] = {VK_FORMAT_UNDEFINED, VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32G32_SFLOAT,
                              VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT};
    return f[n];
}

bool warm_compute(const ShaderInfo* cs, const ProgramLayout& layout, VkShaderModule module) {
    if (!module) return false;
    VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = module;
    stage.pName = "main";
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = stage;
    ci.layout = layout.layout;
    VkPipeline pipeline = VK_NULL_HANDLE;
    const VkResult result = p_vkCreateComputePipelines(vk_device(), pipeline_cache(), 1, &ci, nullptr, &pipeline);
    if (result != VK_SUCCESS) {
        log("nvn/vk: startup compute pipeline for %s failed (%d)", cs->hash.c_str(), int(result));
        return false;
    }
    note_pipeline_cache_changed();
    p_vkDestroyPipeline(vk_device(), pipeline, nullptr);
    return true;
}

bool warm_graphics(const ShaderInfo* vs, const ShaderInfo* fs, const ProgramLayout& layout,
                   VkShaderModule vm, VkShaderModule fm) {
    if (!vs || !vm || (fs && !fm)) return false;
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vm;
    stages[0].pName = "main";
    uint32_t stage_count = 1;
    if (fs) {
        stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fm;
        stages[1].pName = "main";
        stage_count = 2;
    }

    std::vector<VkVertexInputAttributeDescription> attrs;
    uint32_t stride = 0;
    for (const ShaderIo& in : vs->refl.inputs) {
        VkVertexInputAttributeDescription a{};
        a.location = in.location;
        a.binding = 0;
        a.format = io_format(in, false);
        a.offset = stride;
        stride += uint32_t(std::clamp(in.components, 1u, 4u)) * 4;
        attrs.push_back(a);
    }
    VkVertexInputBindingDescription binding{0, stride, VK_VERTEX_INPUT_RATE_VERTEX};
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = attrs.empty() ? 0u : 1u;
    vi.pVertexBindingDescriptions = attrs.empty() ? nullptr : &binding;
    vi.vertexAttributeDescriptionCount = uint32_t(attrs.size());
    vi.pVertexAttributeDescriptions = attrs.data();

    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    rs.rasterizerDiscardEnable = fs == nullptr ? VK_TRUE : VK_FALSE;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    std::vector<VkFormat> formats;
    if (fs) {
        uint32_t count = 0;
        for (const ShaderIo& out : fs->refl.outputs) count = std::max(count, out.location + out.array);
        count = std::min(count, 8u);
        formats.assign(count, VK_FORMAT_R8G8B8A8_UNORM);
        for (const ShaderIo& out : fs->refl.outputs)
            if (out.location < count) formats[out.location] = io_format(out, true);
    }
    std::vector<VkPipelineColorBlendAttachmentState> blend(formats.size());
    for (auto& b : blend) b.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                               VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = uint32_t(blend.size());
    cb.pAttachments = blend.data();
    VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 2;
    dy.pDynamicStates = dynamic_states;
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = uint32_t(formats.size());
    rendering.pColorAttachmentFormats = formats.data();

    VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    ci.pNext = &rendering;
    ci.stageCount = stage_count;
    ci.pStages = stages;
    ci.pVertexInputState = &vi;
    ci.pInputAssemblyState = &ia;
    ci.pViewportState = &vp;
    ci.pRasterizationState = &rs;
    ci.pMultisampleState = &ms;
    ci.pColorBlendState = &cb;
    ci.pDynamicState = &dy;
    ci.layout = layout.layout;
    VkPipeline pipeline = VK_NULL_HANDLE;
    const VkResult result = p_vkCreateGraphicsPipelines(vk_device(), pipeline_cache(), 1, &ci, nullptr, &pipeline);
    if (result != VK_SUCCESS) {
        log("nvn/vk: startup graphics pipeline for %s + %s failed (%d)", vs->hash.c_str(),
            fs ? fs->hash.c_str() : "<no fragment>", int(result));
        return false;
    }
    note_pipeline_cache_changed();
    p_vkDestroyPipeline(vk_device(), pipeline, nullptr);
    return true;
}

bool marker_exists(const std::string& path) {
    FILE* f = plat::fopen_utf8(path, "rb");
    if (!f) return false;
    std::fclose(f);
    return true;
}

void write_marker(const std::string& path, uint64_t fingerprint, uint64_t attempted, uint64_t compiled) {
    if (FILE* f = plat::fopen_utf8(path, "wb")) {
        std::fprintf(f, "catalog=%016llx\nattempted=%llu\ncompiled=%llu\n", (unsigned long long)fingerprint,
                     (unsigned long long)attempted, (unsigned long long)compiled);
        std::fclose(f);
    }
}

}  // namespace

void prepare_shader_catalog() {
    if (std::getenv("NX_SHADER_PRECOMPILE") && std::strcmp(std::getenv("NX_SHADER_PRECOMPILE"), "0") == 0) return;
    std::vector<StageRec> stages;
    std::vector<ProgramRec> programs;
    uint64_t fingerprint = 0;
    if (!read_catalog(stages, programs, fingerprint)) {
        log("nvn/vk: no usable shader program catalog; first-use pipeline compilation remains enabled");
        return;
    }
    VkPhysicalDeviceProperties props{};
    p_vkGetPhysicalDeviceProperties(vk_physical(), &props);
    char marker_name[128];
    // With GPL (nvn_pipeline.cpp) the preparation makes each program's shader libraries instead of a whole pipeline:
    // its own marker, so switching modes prepares again.
    const bool gpl = vk_gpl() && !render_pass_objects();  // as gpl_program_job decides
    std::snprintf(marker_name, sizeof marker_name, "/shader_prepare_%s%04x_%04x_%08x_%016llx.done", gpl ? "gpl_" : "",
                  props.vendorID, props.deviceID, props.driverVersion, (unsigned long long)fingerprint);
    const std::string marker = plat::data_root() + marker_name;
    if (marker_exists(marker)) return;

    const uint64_t total = programs.size();
    prep_progress(0, total, true, 0);
    log("nvn/vk: preparing %llu shader programs before the first frame", (unsigned long long)total);
    pipeline_cache();

    g_catalog_infos.resize(stages.size());
    auto& infos = g_catalog_infos;
    // One module per (stage, unwritten inputs), kept to the end: the batches below compile on worker threads
    // (nvn_pipeline_async.h), so a module still in use must not be replaced.
    std::map<std::pair<uint32_t, uint32_t>, VkShaderModule> modules;
    uint64_t attempted = 0;
    std::atomic<uint64_t> compiled{0}, failed{0};
    std::map<std::vector<const ShaderInfo*>, const ProgramLayout*> layouts;
    std::set<std::vector<uint32_t>> processed;
    const auto get_info = [&](uint32_t index) -> const ShaderInfo* {
        if (index >= stages.size()) return nullptr;
        if (!infos[index]) infos[index] = load_catalog_shader(stages[index]);
        return infos[index].get();
    };
    const auto get_module = [&](uint32_t index, uint32_t unwritten) -> VkShaderModule {
        if (index >= stages.size()) return VK_NULL_HANDLE;
        VkShaderModule& m = modules[{index, unwritten}];
        if (!m)
            if (const ShaderInfo* info = get_info(index)) m = make_module(info, unwritten);
        return m;
    };
    // Programs go to the workers 64 at a time; progress is shown between batches. The cache (100-240 MB) is saved at
    // most every 30 s: read back after every batch, it competed with the workers compiling into it.
    // NX_PREP_SAVE_SECONDS=<n>: another interval (0: after every batch, as before).
    static const uint64_t save_ns = [] {
        const char* e = std::getenv("NX_PREP_SAVE_SECONDS");
        return uint64_t(e ? std::strtoull(e, nullptr, 10) : 30) * 1'000'000'000ull;
    }();
    std::vector<std::function<void()>> batch;
    const uint64_t prep_start = plat::monotonic_ns();
    uint64_t saved_at = prep_start;
    const auto flush = [&] {
        // the compile screen keeps moving while the workers compile (a frame every 33 ms at most)
        run_parallel(batch, [&] { prep_progress(compiled.load(), total, true, 0); });
        batch.clear();
        prep_progress(compiled.load(), total, true, 0);
        if (plat::monotonic_ns() - saved_at >= save_ns) pipeline_cache_save(), saved_at = plat::monotonic_ns();
    };

    for (const ProgramRec& program : programs) {
        if (!processed.insert(program.stages).second) continue;
        attempted++;
        const ShaderInfo* vs = nullptr;
        const ShaderInfo* fs = nullptr;
        const ShaderInfo* cs = nullptr;
        std::vector<const ShaderInfo*> layout_stages;
        for (uint32_t index : program.stages) {
            const ShaderInfo* info = get_info(index);
            if (!info) continue;
            layout_stages.push_back(info);
            if (info->stage == ShaderStage::Vertex) vs = info;
            else if (info->stage == ShaderStage::Fragment) fs = info;
            else if (info->stage == ShaderStage::Compute) cs = info;
        }
        if (layout_stages.empty()) { failed++; continue; }
        auto found = layouts.find(layout_stages);
        const ProgramLayout* layout = nullptr;
        if (found == layouts.end()) {
            layout = &program_layout(layout_stages);
            layouts.emplace(layout_stages, layout);
        } else layout = found->second;

        if (cs) {
            const uint32_t index = *std::find_if(program.stages.begin(), program.stages.end(), [&](uint32_t i) {
                return i < stages.size() && stages[i].stage == ShaderStage::Compute;
            });
            const VkShaderModule module = get_module(index, 0);
            batch.push_back([cs, layout, module, &compiled, &failed] {
                (warm_compute(cs, *layout, module) ? compiled : failed)++;
            });
        } else if (vs && (!fs || fs->stage == ShaderStage::Fragment)) {
            uint32_t vi = UINT32_MAX, fi = UINT32_MAX;
            for (uint32_t i : program.stages) {
                if (i >= stages.size()) continue;
                if (stages[i].stage == ShaderStage::Vertex) vi = i;
                if (stages[i].stage == ShaderStage::Fragment) fi = i;
            }
            if (gpl) {  // the stages' libraries: every pipeline of the program in play is then a fast link
                if (auto job = gpl_program_job(vs, fs, *layout))
                    batch.push_back([job = std::move(job), &compiled] { job(), compiled++; });
                else
                    failed++;
                if (batch.size() >= 64) flush();
                continue;
            }
            const uint32_t unwritten = unwritten_inputs(vs, fs);
            const VkShaderModule vm = get_module(vi, 0), fm = fs ? get_module(fi, unwritten) : VK_NULL_HANDLE;
            batch.push_back([vs, fs, layout, vm, fm, &compiled, &failed] {
                (warm_graphics(vs, fs, *layout, vm, fm) ? compiled : failed)++;
            });
        } else {
            failed++;
        }
        if (batch.size() >= 64) flush();
    }
    if (!batch.empty()) flush();
    for (auto& [stage, module] : modules)
        if (module) p_vkDestroyShaderModule(vk_device(), module, nullptr);
    if (gpl) {  // gpl_program_job made them through module_of: ~0.6 GB kept for the process's life otherwise
        std::set<const ShaderInfo*> catalog;
        for (auto& info : infos)
            if (info) catalog.insert(info.get());
        release_modules(catalog);
    }
    pipeline_cache_save();
    write_marker(marker, fingerprint, attempted, compiled.load());
    prep_progress(compiled.load(), total, false, 0);
    log("nvn/vk: startup shader preparation finished: %llu/%llu programs compiled, %llu failed, in %.1f s",
        (unsigned long long)compiled.load(), (unsigned long long)total, (unsigned long long)failed.load(),
        double(plat::monotonic_ns() - prep_start) / 1e9);
}

}  // namespace nx::nvn
