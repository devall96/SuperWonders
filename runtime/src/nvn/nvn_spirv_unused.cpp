// Declarations the translator emits that no Wonder shader uses, removed when they trip a device limit or feature.
//
// - Fragment shader interlock: 7,839 of the 12,751 shaders declare the FragmentShaderPixelInterlockEXT capability,
//   the SPV_EXT_fragment_shader_interlock extension and an interlock execution mode, and none has a
//   Begin/EndInvocationInterlockEXT. Devices without VK_EXT_fragment_shader_interlock (the AMD drivers) reject the
//   module (VUID-VkShaderModuleCreateInfo-pCode-08740/08742). Removed when no interlock instruction is present.
// - Cull distance: 4,841 vertex shaders declare gl_PerVertex with ClipDistance[8] and CullDistance[1], 9 together,
//   over maxCombinedClipAndCullDistances (8 on AMD; VUID-RuntimeSpirv-maxCombinedClipAndCullDistances-00710). None
//   writes the cull distance. When it is the block's last member and nothing reaches it (no access chain to that
//   member, no whole-block load, store or copy), the member is dropped.
//
// NX_KEEP_UNUSED_DECLS=1: both off (bisecting).
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <vector>

#include "nvn_pipeline.h"
#include "nvn_vk.h"

namespace nx::nvn {

using namespace nx::vk;

namespace {

enum : uint32_t {
    OpExtension = 10, OpExecutionMode = 16, OpCapability = 17, OpTypeArray = 28, OpTypeStruct = 30,
    OpTypePointer = 32, OpConstant = 43, OpVariable = 59, OpLoad = 61, OpStore = 62, OpCopyMemory = 63,
    OpAccessChain = 65, OpInBoundsAccessChain = 66, OpPtrAccessChain = 67, OpMemberName = 6, OpMemberDecorate = 72,
    OpBeginInvocationInterlockEXT = 5364, OpEndInvocationInterlockEXT = 5365,
    BuiltIn = 11, BuiltInClipDistance = 3, BuiltInCullDistance = 4,
};

bool keep_all() {
    static const bool keep = std::getenv("NX_KEEP_UNUSED_DECLS") != nullptr;
    return keep;
}

struct Inst {
    size_t at;
    uint32_t op, len;
};

bool split(const std::vector<uint32_t>& in, std::vector<Inst>& insts) {
    if (in.size() < 5) return false;
    for (size_t i = 5; i < in.size();) {
        uint32_t len = in[i] >> 16;
        if (!len || i + len > in.size()) return false;
        insts.push_back({i, in[i] & 0xffff, len});
        i += len;
    }
    return true;
}

bool interlock_capability(uint32_t cap) { return cap == 5363 || cap == 5372 || cap == 5378; }
bool interlock_mode(uint32_t mode) { return mode >= 5366 && mode <= 5371; }

uint32_t clip_cull_limit() {
    static const uint32_t limit = [] {
        VkPhysicalDeviceProperties p{};
        p_vkGetPhysicalDeviceProperties(vk_physical(), &p);
        return p.limits.maxCombinedClipAndCullDistances ? p.limits.maxCombinedClipAndCullDistances : 8u;
    }();
    return limit;
}

}  // namespace

bool strip_unused_interlock(const std::vector<uint32_t>& in, std::vector<uint32_t>& out) {
    if (keep_all()) return false;  // the runtime never enables VK_EXT_fragment_shader_interlock
    std::vector<Inst> insts;
    if (!split(in, insts)) return false;
    bool declared = false;
    for (const Inst& n : insts) {
        if (n.op == OpBeginInvocationInterlockEXT || n.op == OpEndInvocationInterlockEXT) return false;  // used
        if (n.op == OpCapability && n.len == 2 && interlock_capability(in[n.at + 1])) declared = true;
    }
    if (!declared) return false;
    out.assign(in.begin(), in.begin() + 5);
    for (const Inst& n : insts) {
        const uint32_t* w = &in[n.at];
        if (n.op == OpCapability && n.len == 2 && interlock_capability(w[1])) continue;
        if (n.op == OpExecutionMode && n.len >= 3 && interlock_mode(w[2])) continue;
        if (n.op == OpExtension && n.len >= 2 &&
            std::strncmp(reinterpret_cast<const char*>(w + 1), "SPV_EXT_fragment_shader_interlock",
                         (n.len - 1) * 4) == 0)
            continue;
        out.insert(out.end(), w, w + n.len);
    }
    return true;
}

bool drop_unused_cull_distance(const std::vector<uint32_t>& in, std::vector<uint32_t>& out) {
    if (keep_all()) return false;
    std::vector<Inst> insts;
    if (!split(in, insts)) return false;
    std::map<uint32_t, uint32_t> constant, array_len, struct_at;  // id -> value / length id / instruction index
    std::map<uint32_t, uint32_t> pointee;                          // pointer type -> pointee
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> member_builtin;
    for (size_t k = 0; k < insts.size(); k++) {
        const Inst& n = insts[k];
        const uint32_t* w = &in[n.at];
        if (n.op == OpConstant && n.len == 4) constant[w[2]] = w[3];
        else if (n.op == OpTypeArray && n.len == 4) array_len[w[1]] = w[3];
        else if (n.op == OpTypeStruct) struct_at[w[1]] = uint32_t(k);
        else if (n.op == OpTypePointer && n.len == 4) pointee[w[1]] = w[3];
        else if (n.op == OpMemberDecorate && n.len >= 5 && w[3] == BuiltIn) member_builtin[{w[1], w[2]}] = w[4];
    }
    auto member_count = [&](uint32_t s) { return insts[struct_at[s]].len - 2; };
    auto member_array = [&](uint32_t s, uint32_t m) -> uint32_t {
        uint32_t t = in[insts[struct_at[s]].at + 2 + m];
        auto a = array_len.find(t);
        if (a == array_len.end()) return 0;
        auto c = constant.find(a->second);
        return c == constant.end() ? 0 : c->second;
    };
    // Blocks whose last member is a CullDistance over the limit together with their ClipDistance.
    std::set<uint32_t> blocks;
    for (auto& [s, k] : struct_at) {
        const uint32_t count = member_count(s);
        if (!count) continue;
        auto cull = member_builtin.find({s, count - 1});
        if (cull == member_builtin.end() || cull->second != BuiltInCullDistance) continue;
        uint32_t clip_len = 0;
        for (uint32_t m = 0; m + 1 < count; m++)
            if (auto c = member_builtin.find({s, m}); c != member_builtin.end() && c->second == BuiltInClipDistance)
                clip_len = member_array(s, m);
        if (clip_len + member_array(s, count - 1) > clip_cull_limit()) blocks.insert(s);
    }
    if (blocks.empty()) return false;
    // Variables of those blocks; anything that reaches the cull member, or the whole block, keeps the block as it is.
    std::map<uint32_t, uint32_t> block_of_var;
    for (const Inst& n : insts)
        if (n.op == OpVariable && n.len >= 4)
            if (auto p = pointee.find(in[n.at + 1]); p != pointee.end() && blocks.count(p->second))
                block_of_var[in[n.at + 2]] = p->second;
    for (const Inst& n : insts) {
        const uint32_t* w = &in[n.at];
        if ((n.op == OpAccessChain || n.op == OpInBoundsAccessChain) && n.len >= 5 && block_of_var.count(w[3])) {
            const uint32_t s = block_of_var[w[3]];
            auto c = constant.find(w[4]);
            if (c == constant.end() || c->second == member_count(s) - 1) blocks.erase(s);  // dynamic or cull index
        } else if (n.op == OpPtrAccessChain && n.len >= 4 && block_of_var.count(w[3])) {
            blocks.erase(block_of_var[w[3]]);
        } else if (n.op == OpLoad && n.len >= 4 && block_of_var.count(w[3])) {
            blocks.erase(block_of_var[w[3]]);
        } else if ((n.op == OpStore || n.op == OpCopyMemory) && n.len >= 3 &&
                   (block_of_var.count(w[1]) || block_of_var.count(w[2]))) {
            blocks.erase(block_of_var.count(w[1]) ? block_of_var[w[1]] : block_of_var[w[2]]);
        }
    }
    if (blocks.empty()) return false;
    out.assign(in.begin(), in.begin() + 5);
    for (const Inst& n : insts) {
        const uint32_t* w = &in[n.at];
        if (n.op == OpTypeStruct && blocks.count(w[1])) {
            out.push_back(((n.len - 1) << 16) | OpTypeStruct);
            out.insert(out.end(), w + 1, w + n.len - 1);
            continue;
        }
        if ((n.op == OpMemberDecorate || n.op == OpMemberName) && n.len >= 3 && blocks.count(w[1]) &&
            w[2] == member_count(w[1]) - 1)
            continue;
        out.insert(out.end(), w, w + n.len);
    }
    return true;
}

}  // namespace nx::nvn
