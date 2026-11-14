cmake_minimum_required(VERSION 3.25)
project(SwitchGamesRecomp C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The game this build is for: games/<NX_GAME>/ holds what only that game needs (game.cpp: nx/game.h), the shared
# runtime is the rest. One build directory per game.
set(NX_GAME "hades" CACHE STRING "Game to build (a folder under games/)")
set(NX_GAME_DIR "${CMAKE_SOURCE_DIR}/games/${NX_GAME}")
if(WIN32 AND NX_GAME STREQUAL "wonder")
    enable_language(RC)
endif()
if(NOT EXISTS "${NX_GAME_DIR}/game.cpp")
    message(FATAL_ERROR "games/${NX_GAME}/game.cpp not found")
endif()

# Generated code of the game (python -m recomp <nso> <gen dir>/<module> --name <module> --base ...). Never committed.
# Hades keeps the layout from before there were other games (private/gen); the others use private/<game>/gen.
if(NX_GAME STREQUAL "hades")
    set(NX_GEN_DEFAULT "${CMAKE_SOURCE_DIR}/private/gen")
else()
    set(NX_GEN_DEFAULT "${CMAKE_SOURCE_DIR}/private/${NX_GAME}/gen")
endif()
set(NX_GEN_DIR "${NX_GEN_DEFAULT}" CACHE PATH "Recompiled module sources")

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_compile_options(-Wno-unused-label -Wno-unused-value -Wno-parentheses-equality -Wno-unused-variable
                        -Wno-unused-but-set-variable -Wno-constant-logical-operand -Wno-unused-function)
endif()
if(MSVC)
    add_compile_options(/arch:AVX2)
elseif(MINGW)
    add_compile_options(-mavx2)
endif()
if(WIN32)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# Vulkan headers (the same registry version tools/gen_vk.py generated from). The loader comes with the GPU driver.
if(DEFINED ENV{VULKAN_SDK})
    set(NX_VULKAN_SDK "$ENV{VULKAN_SDK}" CACHE PATH "Vulkan SDK")
else()
    set(NX_VULKAN_SDK "C:/VulkanSDK/1.4.357.0" CACHE PATH "Vulkan SDK")
endif()

# The runtime's own compute shaders (runtime/shaders), compiled to SPIR-V here and #included as word lists: nothing is
# compiled when the game runs. glslc comes with the Vulkan SDK (the NDK's shader-tools on Android, shaderc on macOS).
set(NX_SHADER_OUT "${CMAKE_BINARY_DIR}/shaders")
file(GLOB NX_NDK_SHADER_TOOLS "${ANDROID_NDK}/shader-tools/*")
find_program(NX_GLSLC glslc HINTS "${NX_VULKAN_SDK}/Bin" "${NX_VULKAN_SDK}/bin" ${NX_NDK_SHADER_TOOLS} REQUIRED)
foreach(shader astc_decode.comp)
    set(out "${NX_SHADER_OUT}/${shader}.inc")
    add_custom_command(OUTPUT "${out}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${NX_SHADER_OUT}"
        COMMAND "${NX_GLSLC}" -O --target-env=vulkan1.0 -mfmt=num -o "${out}" "${CMAKE_SOURCE_DIR}/runtime/shaders/${shader}"
        DEPENDS "${CMAKE_SOURCE_DIR}/runtime/shaders/${shader}"
        COMMENT "glslc ${shader}")
    list(APPEND NX_SHADER_SOURCES "${out}")
endforeach()

# Unit tests of runtime pieces that build without the game (docs/nvn/WINDOWS-PLAN.md, "Test plan"): ctest.
option(NX_TESTS "Build the standalone unit tests" OFF)
if(NX_TESTS AND NOT ANDROID)
    enable_testing()
    add_executable(gpu_pick_test tests/gpu_pick_test.cpp runtime/src/nvn/nvn_gpu_pick.cpp)
    target_compile_definitions(gpu_pick_test PRIVATE NX_GPU_PICK_STANDALONE)
    target_include_directories(gpu_pick_test PRIVATE runtime/src/nvn)
    add_test(NAME gpu_pick COMMAND gpu_pick_test)
    add_executable(tiling_test tests/tiling_test.cpp runtime/src/nvn/nvn_tiling.cpp)
    target_include_directories(tiling_test PRIVATE runtime/src/nvn)
    add_test(NAME tiling COMMAND tiling_test --selftest)
    add_executable(astc_test tests/astc_test.cpp runtime/src/nvn/nvn_astc.cpp)
    target_include_directories(astc_test PRIVATE runtime/src/nvn)
    add_test(NAME astc COMMAND astc_test --selftest)
    if(WIN32)  # the GPU decoder against the CPU one (needs a Vulkan device)
        add_executable(astc_gpu_test tests/astc_gpu_test.cpp runtime/src/nvn/nvn_astc.cpp
                       "${NX_SHADER_OUT}/astc_decode.comp.inc")
        target_include_directories(astc_gpu_test PRIVATE runtime/src/nvn "${NX_SHADER_OUT}" "${NX_VULKAN_SDK}/Include")
        target_link_libraries(astc_gpu_test PRIVATE "${NX_VULKAN_SDK}/Lib/vulkan-1.lib")
        add_test(NAME astc_gpu COMMAND astc_gpu_test)
    endif()
    add_executable(shader_cache_test tests/shader_cache_test.cpp runtime/src/nvn/nvn_shaders.cpp
                   runtime/src/nvn/nvn_spirv_reflect.cpp)
    target_compile_definitions(shader_cache_test PRIVATE NX_SHADERS_STANDALONE)
    target_include_directories(shader_cache_test PRIVATE runtime/src/nvn)
    if(EXISTS "${CMAKE_SOURCE_DIR}/private/wonder/spirv")  # the game's own shaders: never in the repository
        add_test(NAME shader_cache COMMAND shader_cache_test "${CMAKE_SOURCE_DIR}/private/wonder")
    endif()
    if(WIN32)
        add_executable(memguard_test tests/memguard_test.cpp)
        target_link_libraries(memguard_test PRIVATE psapi)
        add_test(NAME memguard COMMAND memguard_test)
    endif()
endif()

# Without the generated code (a checkout with no game data) only the tests above are built.
file(GLOB NX_GEN_PROBE "${NX_GEN_DIR}/main/*.cpp")
if(NOT NX_GEN_PROBE)
    message(WARNING "No generated code in ${NX_GEN_DIR}/main: building the unit tests only")
    return()
endif()

foreach(mod main sdk)
    file(GLOB src CONFIGURE_DEPENDS "${NX_GEN_DIR}/${mod}/*.cpp")
    add_library(game_${mod} STATIC ${src})
    target_include_directories(game_${mod} PRIVATE runtime/include "${NX_GEN_DIR}/${mod}")
    if(ANDROID)
        # Hidden visibility: calls between recompiled functions are direct, not through the PLT.
        # armv8.2-a+crypto: cpu.h's AES/SHA paths need __ARM_FEATURE_AES/SHA2 (the NDK default armv8-a has neither).
        target_compile_options(game_${mod} PRIVATE -O2 -fPIC -fvisibility=hidden -fno-semantic-interposition
                               -march=armv8.2-a+crypto)
    elseif(APPLE)
        target_compile_options(game_${mod} PRIVATE -O2 -fvisibility=hidden)
    endif()
endforeach()

set(NX_RUNTIME_SOURCES
    runtime/src/main.cpp
    runtime/src/loader.cpp
    runtime/src/memory.cpp
    runtime/src/kernel.cpp
    runtime/src/kernel_watch.cpp
    runtime/src/ipc.cpp
    runtime/src/svc_am.cpp
    runtime/src/svc_misc.cpp
    runtime/src/svc_time.cpp
    runtime/src/svc_process.cpp
    runtime/src/hle_fs.cpp
    runtime/src/svc_account.cpp
    runtime/src/hle_ro.cpp
    runtime/src/hle_vi.cpp
    runtime/src/hle_hid.cpp
    runtime/src/hle_net.cpp
    runtime/src/hle_audio.cpp
    runtime/src/hle_pl.cpp
    runtime/src/input_script.cpp
    runtime/src/touch_layout.cpp
    runtime/src/perf_hint.cpp
    runtime/src/frame_stats.cpp
    runtime/src/write_watch.cpp
    runtime/src/nvn/nvn_caps.cpp
    runtime/src/nvn/nvn_renderpass.cpp
    runtime/src/nvn/nvn_gpu_busy.cpp
    runtime/src/nvn/nvn_janitor.cpp
    runtime/src/nvn/nvn_trace.cpp
    runtime/src/nvn/nvn_device.cpp
    runtime/src/nvn/nvn_objects.cpp
    runtime/src/nvn/nvn_incarnation.cpp
    runtime/src/nvn/nvn_texture_layout.cpp
    runtime/src/nvn/nvn_program.cpp
    runtime/src/nvn/nvn_queue.cpp
    runtime/src/nvn/nvn_cmd.cpp
    runtime/src/nvn/nvn_state.cpp
    runtime/src/nvn/nvn_format.cpp
    runtime/src/nvn/nvn_enums.cpp
    runtime/src/nvn/nvn_sampler.cpp
    runtime/src/nvn/nvn_sampler_objects.cpp
    runtime/src/nvn/nvn_draw_state.cpp
    runtime/src/nvn/nvn_raster.cpp
    runtime/src/nvn/nvn_worker.cpp
    runtime/src/nvn/nvn_shaders.cpp
    runtime/src/nvn/nvn_shader_prepare.cpp
    runtime/src/nvn/nvn_spirv_reflect.cpp
    runtime/src/nvn/nvn_tiling.cpp
    runtime/src/nvn/nvn_vk.cpp
    runtime/src/nvn/nvn_gpu_pick.cpp
    runtime/src/nvn/nvn_image.cpp
    runtime/src/nvn/nvn_frames.cpp
    runtime/src/nvn/nvn_dedup.cpp
    runtime/src/nvn/nvn_written.cpp
    runtime/src/nvn/nvn_replay.cpp
    runtime/src/nvn/nvn_debug.cpp
    runtime/src/nvn/nvn_binds.cpp
    runtime/src/nvn/nvn_copy.cpp
    runtime/src/nvn/nvn_pipeline.cpp
    runtime/src/nvn/nvn_dummy.cpp
    runtime/src/nvn/nvn_pipeline_cache.cpp
    runtime/src/nvn/nvn_upload.cpp
    runtime/src/nvn/nvn_astc.cpp
    runtime/src/nvn/nvn_spirv_patch.cpp
    runtime/src/nvn/nvn_spirv_int_textures.cpp
    runtime/src/nvn/nvn_spirv_locals.cpp
    runtime/src/nvn/nvn_spirv_clip.cpp
    runtime/src/nvn/nvn_present.cpp
    runtime/src/nvn/nvn_pipeline_cache.cpp
    runtime/src/nvn/nvn_bc.cpp
    runtime/src/nvn/nvn_draw.cpp
    runtime/src/nvn/nvn_sets.cpp
    runtime/src/nvn/nvn_clears.cpp
    runtime/src/nvn/nvn_mask_clear.cpp
    runtime/src/nvn/nvn_draw_trace.cpp
    runtime/src/vk_bridge.cpp
    runtime/src/vk_swapchain.cpp
    runtime/src/vk_gen.cpp
    runtime/src/vk_present.cpp
)
file(GLOB NX_GAME_SOURCES CONFIGURE_DEPENDS "${NX_GAME_DIR}/*.cpp")
list(APPEND NX_RUNTIME_SOURCES ${NX_GAME_SOURCES} ${NX_SHADER_SOURCES})

if(ANDROID)
    # NativeActivity app: lib<game>.so, entry android_main (android_native_app_glue).
    set(GLUE "${ANDROID_NDK}/sources/android/native_app_glue")
    add_library(${NX_GAME} SHARED ${NX_RUNTIME_SOURCES} runtime/src/platform_android.cpp
        runtime/src/platform_android_input.cpp runtime/src/platform_android_audio.cpp runtime/src/memguard_android.cpp
        "${GLUE}/android_native_app_glue.c")
    target_include_directories(${NX_GAME} PRIVATE runtime/include runtime/src "${NX_GAME_DIR}" "${NX_VULKAN_SDK}/Include" "${GLUE}")
    # Our ANativeActivity_onCreate (platform_android.cpp) runs first, on the UI thread, then calls the glue's.
    set_source_files_properties("${GLUE}/android_native_app_glue.c" PROPERTIES
        COMPILE_DEFINITIONS "ANativeActivity_onCreate=glue_ANativeActivity_onCreate")
    target_link_libraries(${NX_GAME} PRIVATE game_main game_sdk android log aaudio)
    target_compile_options(${NX_GAME} PRIVATE -fvisibility=hidden -fno-semantic-interposition -march=armv8.2-a+crypto)
    # The glue's ANativeActivity_onCreate must be exported. 16 KB page alignment: Android 15+ devices with 16 KB pages.
    target_link_options(${NX_GAME} PRIVATE -u ANativeActivity_onCreate -Wl,-z,max-page-size=16384)
elseif(APPLE)
    # macOS: SDL3 (window, input, audio) and the Vulkan loader + MoltenVK from Homebrew
    # (brew install sdl3 vulkan-headers vulkan-loader molten-vk shaderc).
    find_package(SDL3 REQUIRED CONFIG)
    find_path(NX_VULKAN_INCLUDE vulkan/vulkan.h HINTS /opt/homebrew/include /usr/local/include)
    add_executable(${NX_GAME} ${NX_RUNTIME_SOURCES} runtime/src/platform_mac.cpp runtime/src/memguard_mac.cpp)
    target_include_directories(${NX_GAME} PRIVATE runtime/include runtime/src "${NX_GAME_DIR}" "${NX_VULKAN_INCLUDE}")
    target_link_libraries(${NX_GAME} PRIVATE game_main game_sdk SDL3::SDL3)
    target_compile_options(${NX_GAME} PRIVATE -fvisibility=hidden)
    # The executable loads near 0x1_0000_0000, below the guest ranges (0x40_0000_0000 and up). The game runs on its
    # own thread with a 64 MiB stack (platform_mac.cpp), so the main thread's stack size does not matter.
else()
    if(WIN32)
        # SDL's HIDAPI gamepad backend provides native DualSense support without a driver wrapper.
        include(FetchContent)
        set(SDL_SHARED OFF CACHE BOOL "" FORCE)
        set(SDL_STATIC ON CACHE BOOL "" FORCE)
        set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
        set(SDL_TESTS OFF CACHE BOOL "" FORCE)
        set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
        set(SDL3_BUNDLED_SOURCE "${CMAKE_SOURCE_DIR}/tools/portable/source/SDL3-3.4.18")
        if(EXISTS "${SDL3_BUNDLED_SOURCE}/CMakeLists.txt")
            # FetchContent's explicit source override avoids interpreting a Windows path as a file URL.
            set(FETCHCONTENT_SOURCE_DIR_SDL3 "${SDL3_BUNDLED_SOURCE}" CACHE PATH "Bundled SDL source" FORCE)
        endif()
        FetchContent_Declare(SDL3
            URL https://github.com/libsdl-org/SDL/releases/download/release-3.4.18/SDL3-3.4.18.tar.gz
            URL_HASH SHA256=9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
        FetchContent_MakeAvailable(SDL3)
    endif()
    add_executable(${NX_GAME} ${NX_RUNTIME_SOURCES} runtime/src/platform_win.cpp runtime/src/pc_menu_win.cpp
                   runtime/src/memguard_win.cpp)
    target_include_directories(${NX_GAME} PRIVATE runtime/include runtime/src "${NX_GAME_DIR}" "${NX_VULKAN_SDK}/Include")
    if(MINGW)
        # LLVM-MinGW ships the XAudio2 2.9 import library under its SDK name. Its plain "xinput" is XInput 1.3 (the
        # DirectX 2010 redistributable, missing on a clean Windows); 1.4 ships with Windows 8 and later.
        target_link_libraries(${NX_GAME} PRIVATE game_main game_sdk xinput1_4 xaudio2_9 ole32 psapi)
        # libc++ and libunwind linked in: without it wonder.exe needs libc++.dll and libunwind.dll next to it, which
        # nothing copies there (the portable pack copies the MSVC runtime). The UCRT stays a system DLL.
        target_link_options(${NX_GAME} PRIVATE -static)
    else()
        target_link_libraries(${NX_GAME} PRIVATE game_main game_sdk xinput xaudio2 ole32 psapi)
    endif()
    if(WIN32)
        target_link_libraries(${NX_GAME} PRIVATE SDL3::SDL3-static)
        if(NX_GAME STREQUAL "wonder")
            file(TO_CMAKE_PATH "${CMAKE_SOURCE_DIR}/assets/wonder_app.ico" WONDER_APP_ICON_PATH)
            configure_file(assets/wonder_app.rc.in "${CMAKE_CURRENT_BINARY_DIR}/wonder_app.rc" @ONLY)
            target_sources(${NX_GAME} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/wonder_app.rc")
        endif()
    endif()
    # The runtime uses the portable C library (getenv, fopen, ...) on every host: no "_s" variants.
    target_compile_definitions(${NX_GAME} PRIVATE _CRT_SECURE_NO_WARNINGS _SILENCE_CXX20_U8PATH_DEPRECATION_WARNING)
    if(MSVC)
        # Keep host allocations low so the guest ranges (0x40_0000_0000..0x70_0000_0000) are always free; a big
        # main-thread stack (recompiled calls nest deeply); a link map for tools/symbolize.py.
        target_link_options(${NX_GAME} PRIVATE /STACK:67108864 /HIGHENTROPYVA:NO /MAP)
    elseif(MINGW)
        # llvm-mingw portable-toolchain equivalents of the MSVC linker options above.
        target_link_options(${NX_GAME} PRIVATE -Wl,--stack,67108864 -Wl,--disable-high-entropy-va
                            "-Wl,-Map,$<TARGET_FILE_DIR:${NX_GAME}>/${NX_GAME}.map")
    endif()
endif()

target_include_directories(${NX_GAME} PRIVATE "${NX_SHADER_OUT}")
