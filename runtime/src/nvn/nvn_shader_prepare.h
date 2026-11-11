#pragma once

namespace nx::nvn {

// Reads the local game-derived shader program catalog and creates native Vulkan pipelines once.
void prepare_shader_catalog();

}  // namespace nx::nvn
