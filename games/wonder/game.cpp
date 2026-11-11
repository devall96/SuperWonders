// Super Mario Bros. Wonder (010015100B514000, 1.0.0): what the shared runtime needs to know about the game.
// The game draws through NVN (docs/NVN.md); what we know about it is in docs/nvn/WONDER.md.
#include <nx/game.h>
#include <nx/runtime.h>
#include <nx/module.h>

#include <cstring>

#include "platform.h"

extern const nx::ModuleInfo nx_module_main;

namespace nx {

const game::Info game::kInfo = {"wonder", "Super Mario Bros. Wonder", 0x010015100B514000ull,
                                "io.github.belmantegu.wonderrecomp", "private/wonder",
                                0x1000000 /* main.npdm: 16 MiB system resource */};

namespace {

static uint32_t float_bits(float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

// Gather evidence about Wonder's own virtual-screen globals before implementing Hor+.
// The final image pass can only fit, crop, or stretch its 16:9 source; finding and
// classifying these game-side values is needed to expand the camera while preserving HUD layout.
static void trace_screen_globals() {
    if (plat::setting("wonder_widescreen_trace") != "1") return;
    const uint64_t begin = nx_module_main.base + nx_module_main.data;
    const uint64_t end = nx_module_main.base + nx_module_main.bss + nx_module_main.bss_size;
    const uint32_t pairs[][2] = {
        {1920, 1080}, {1280, 720}, {960, 540}, {640, 360},
        {float_bits(1920.0f), float_bits(1080.0f)},
        {float_bits(1280.0f), float_bits(720.0f)},
        {float_bits(960.0f), float_bits(540.0f)},
        {float_bits(640.0f), float_bits(360.0f)},
    };
    constexpr const char* names[] = {"1920x1080 ints", "1280x720 ints", "960x540 ints", "640x360 ints",
                                    "1920x1080 floats", "1280x720 floats", "960x540 floats", "640x360 floats"};
    log("widescreen probe: scanning main data+bss [%llx,%llx)", (unsigned long long)begin,
        (unsigned long long)end);
    for (size_t p = 0; p < sizeof(pairs) / sizeof(pairs[0]); ++p) {
        unsigned count = 0;
        for (uint64_t address = begin; address + 8 <= end; address += 4) {
            const auto* words = reinterpret_cast<const uint32_t*>(address);
            if (words[0] != pairs[p][0] || words[1] != pairs[p][1]) continue;
            if (count < 24)
                log("widescreen probe: %s candidate main+%llx", names[p],
                    (unsigned long long)(address - nx_module_main.base));
            ++count;
        }
        log("widescreen probe: %s count=%u", names[p], count);
    }
}

}  // namespace

void game::after_init() { trace_screen_globals(); }
void game::renderer_start() {}
void game::first_frame() {}

}  // namespace nx
