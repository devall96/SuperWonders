// Graphics pipelines compiled on worker threads (nvn_pipeline_async.cpp).
//
// A pipeline a draw needs for the first time was compiled on the replay thread, and the game thread waited on it:
// on the AMD Windows driver a course's first minutes compiled ~2,000 pipelines in 16.5 s, with single stalls up to a
// second. Now graphics_pipeline() can hand the compile to a worker and skip the draw meanwhile, when that is safe:
// every target of the pass was also rendered in the previous frame, so the next frame draws it again (the TotK
// recomp's rule, valentderah/totk-pc-recomp docs/architecture.md, MIT). A one-shot pass (a target rendered once, a
// light probe) still waits, so nothing is lost. The startup shader preparation runs its pipelines on the same pool.
#pragma once

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#include "nvn_pipeline.h"

namespace nx::nvn {

// Everything vkCreateGraphicsPipelines reads, in one heap object that outlives graphics_pipeline() while a worker
// compiles it. Never copied: the create info points into its own members.
struct PipelineBuild {
    PipelineKey key;  // a copy: ri.pColorAttachmentFormats points into it
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
    bool ready() {
        std::lock_guard l(lock);
        return done;
    }
    void wait() {
        std::unique_lock l(lock);
        cv.wait(l, [&] { return done; });
    }
};

bool async_pipelines();  // NX_ASYNC_PIPELINES=0: every pipeline compiled where it is needed, as before (bisecting)
extern uint64_t g_pipelines_skipped;  // draws skipped while their pipeline compiled (replay thread; logged per window)
std::shared_ptr<AsyncPipeline> compile_pipeline_async(std::shared_ptr<PipelineBuild> build);
// Runs every job on the worker pool and returns when all have finished.
void run_parallel(std::vector<std::function<void()>>& jobs);

}  // namespace nx::nvn
