// Graphics pipelines compiled on worker threads (nvn_pipeline_async.cpp).
//
// A pipeline a draw needs for the first time was compiled on the replay thread, and the game thread waited on it:
// on the AMD Windows driver a course's first minutes compiled ~2,000 pipelines in 16.5 s, with single stalls up to a
// second. Now graphics_pipeline() can hand the compile to a worker and skip the draw meanwhile, when that is safe:
// every target of the pass was also rendered in the previous frame, so the next frame draws it again (the TotK
// recomp's rule, valentderah/totk-pc-recomp docs/architecture.md, MIT). A one-shot pass (a target rendered once, a
// light probe) still waits, so nothing is lost. The startup shader preparation runs its pipelines on the same pool.
#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "nvn_pipeline.h"

namespace nx::nvn {

// Everything vkCreateGraphicsPipelines reads, in one heap object that outlives graphics_pipeline() while a worker
// compiles it. Never copied: the create info points into its own members.
struct PipelineBuild {
    PipelineKey key;  // a copy: ri.pColorAttachmentFormats points into it
    std::string vs_id, fs_id;  // the stages' module identities (hash and patches), for the GPL library keys
    VkPipelineShaderStageCreateInfo stages[2]{};
    std::vector<VkVertexInputAttributeDescription> attrs;
    std::vector<VkVertexInputBindingDescription> binds;
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    VkPipelineViewportDepthClipControlCreateInfoEXT clip{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLIP_CONTROL_CREATE_INFO_EXT};
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState att[8]{};
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    PipelineBuild() = default;
    PipelineBuild(const PipelineBuild&) = delete;
    PipelineBuild& operator=(const PipelineBuild&) = delete;
};

// A compile handed to a worker. `done` turns true once, under `lock`; then `result`, `pipeline` and `ns` are final.
struct AsyncPipeline {
    std::shared_ptr<PipelineBuild> build;
    std::mutex lock;
    std::condition_variable cv;
    bool done = false;
    VkResult result = VK_NOT_READY;
    VkPipeline pipeline = VK_NULL_HANDLE;
    uint64_t ns = 0;  // the compile's own time on the worker
    std::atomic<bool> finished{false};  // `done` without the lock (polled per draw by the GPL swap, nvn_pipeline.cpp)
    bool ready() {
        std::lock_guard l(lock);
        return done;
    }
    void wait() {
        std::unique_lock l(lock);
        cv.wait(l, [&] { return done; });
    }
};

// NX_ASYNC_PIPELINES=0: every pipeline compiled where it is needed, and the startup preparation one program at a
// time on its own thread, as before (bisecting).
bool async_pipelines();
extern uint64_t g_pipelines_skipped;  // draws skipped while their pipeline compiled (replay thread; logged per window)
std::shared_ptr<AsyncPipeline> compile_pipeline_async(std::shared_ptr<PipelineBuild> build);
// A job on the same worker pool (the GPL libraries a skipped draw's program needs, nvn_pipeline.cpp).
void run_on_pool(std::function<void()> job);
// Runs every job on the worker pool and returns when all have finished.
void run_parallel(std::vector<std::function<void()>>& jobs, const std::function<void()>& tick = {});

// Disk writes off the replay thread: the periodic saves of the pipeline cache (~240 MB) and the pipeline recipes
// (~9 MB) took 100-600 ms each, a stall every 10 s. One thread, jobs in order. wait_background_writes() returns once
// all queued jobs are done (the shutdown saves write synchronously after it).
void background_write(std::function<void()> job);
void wait_background_writes();

}  // namespace nx::nvn
