"""Wonder 1.0.0 frame-step patch, based on the game's verified AArch64 instructions.

Wonder normally turns the NVN present interval (1 or 2) into its simulation step, so
the game runs at the wrong speed if it is rendered at a rate other than 60/30 FPS.
Replace those calculations with loads from a runtime-owned page. The runtime updates
that page from the measured elapsed time between presents.
"""
import struct


BRANCHES = (0xC9528, 0xC9588)
SITE_WORDS = 8
ORIGINAL = (
    0xBD4262C0, 0x52A84E08, 0xAA1F03E9, 0x1E270101,
    0x91022288, 0x52AFF80A, 0x5E21D800, 0x1E211801,
)

ADRP8, LOAD_RATIO, LOAD_SECONDS, NOP = -1, -2, -3, -4
REPLACEMENT = (ADRP8, LOAD_RATIO, LOAD_SECONDS, 2, 4, 5, NOP, NOP)


def _adrp(rd, pc, target):
    pages = (target >> 12) - (pc >> 12)
    if not -(1 << 20) <= pages < (1 << 20):
        raise ValueError("Wonder real-time frame-step page is outside ADRP range")
    imm = pages & 0x1FFFFF
    return 0x90000000 | ((imm & 3) << 29) | ((imm >> 2) << 5) | rd


def patch_wonder_realtime(module):
    """Patch main's two present-interval conversions and reserve one guest data page."""
    if module.name != "main":
        return

    bss_start, bss_size = module.bss
    page = (bss_start + bss_size + 0xFFF) & ~0xFFF
    if page < module.text[0] + module.text[1] and page + 0x1000 > module.text[0]:
        raise ValueError("Wonder real-time frame-step page overlaps main text")

    for branch in BRANCHES:
        if branch < module.text[0] or branch + SITE_WORDS * 4 > module.text[0] + module.text[1]:
            raise ValueError(f"Wonder real-time frame-step site main+{branch:#x} is outside main text")
        actual = struct.unpack_from("<" + "I" * SITE_WORDS, module.image, branch)
        if actual != ORIGINAL:
            raise ValueError(
                f"Wonder real-time frame-step patch expects game version 1.0.0: "
                f"main+{branch:#x} has {actual!r}"
            )

    for branch in BRANCHES:
        words = []
        for i, item in enumerate(REPLACEMENT):
            if item == ADRP8:
                word = _adrp(8, branch + i * 4, page)
            elif item == LOAD_RATIO:
                word = 0xBD400100  # ldr s0, [x8]
            elif item == LOAD_SECONDS:
                word = 0xBD400501  # ldr s1, [x8, #4]
            elif item == NOP:
                word = 0xD503201F
            else:
                word = ORIGINAL[item]
            words.append(word)
        struct.pack_into("<" + "I" * SITE_WORDS, module.image, branch, *words)

    # load_modules() commits through the end of BSS. Put the page at the end so the
    # runtime can find it through the optional ModuleInfo offset.
    module.bss = (bss_start, page + 0x1000 - bss_start)
    module.frame_step_offset = page
