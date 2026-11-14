// Function-local variables start at 0.
//
// The translator keeps Maxwell registers in function-local variables, and a register the game's code reads before any
// write on some path (paths the structurizer adds included) becomes an OpVariable loaded before it is stored. SPIR-V
// leaves that value undefined; NVIDIA returned 0, as the Maxwell code expects. AMD did not: the course ground and the
// characters' lighting came out flat green on the AMD Windows driver and black on RADV (Mesa folds undefined values
// freely), while llvmpipe and the same shader recompiled from GLSL drew them right. Wonder's ground shaders hold ~200
// such variables each. Every function variable without an initializer gets OpConstantNull of its type.
// The TotK recomp found the same rule (valentderah/totk-pc-recomp, docs/maxwell.md 3.12, translator patch 0016, MIT).
// NX_ZERO_LOCALS=0: off (bisecting).
#include <algorithm>
#include <cstdlib>
#include <map>
#include <vector>

#include "nvn_pipeline.h"

namespace nx::nvn {

bool zero_function_locals(const std::vector<uint32_t>& in, std::vector<uint32_t>& out) {
    enum : uint32_t { OpTypePointer = 32, OpConstantNull = 46, OpFunction = 54, OpVariable = 59, StorageFunction = 7 };
    static const bool on = [] {
        const char* e = std::getenv("NX_ZERO_LOCALS");
        return !(e && e[0] == '0');
    }();
    if (!on || in.size() < 5) return false;
    struct Inst {
        size_t at;
        uint32_t op, len;
    };
    std::vector<Inst> insts;
    for (size_t i = 5; i < in.size();) {
        uint32_t len = in[i] >> 16;
        if (!len || i + len > in.size()) return false;
        insts.push_back({i, in[i] & 0xffff, len});
        i += len;
    }
    std::map<uint32_t, uint32_t> pointee;  // Function pointer type -> the type it points to
    size_t first_function = in.size();
    for (const Inst& n : insts) {
        const uint32_t* w = &in[n.at];
        if (n.op == OpTypePointer && n.len == 4 && w[2] == StorageFunction) pointee[w[1]] = w[3];
        if (n.op == OpFunction) {
            first_function = n.at;
            break;
        }
    }
    uint32_t bound = in[3];
    std::map<uint32_t, uint32_t> null_of;  // type -> its OpConstantNull
    for (const Inst& n : insts)
        if (n.at >= first_function && n.op == OpVariable && n.len == 4 && in[n.at + 3] == StorageFunction)
            if (auto p = pointee.find(in[n.at + 1]); p != pointee.end() && !null_of.count(p->second))
                null_of[p->second] = bound++;
    if (null_of.empty()) return false;
    out.assign(in.begin(), in.begin() + first_function);
    for (auto [type, id] : null_of) out.insert(out.end(), {(3u << 16) | OpConstantNull, type, id});
    for (const Inst& n : insts) {
        if (n.at < first_function) continue;
        const uint32_t* w = &in[n.at];
        if (n.op == OpVariable && n.len == 4 && w[3] == StorageFunction)
            if (auto p = pointee.find(w[1]); p != pointee.end()) {
                out.insert(out.end(), {(5u << 16) | OpVariable, w[1], w[2], w[3], null_of[p->second]});
                continue;
            }
        out.insert(out.end(), w, w + n.len);
    }
    out[3] = bound;
    return true;
}

}  // namespace nx::nvn
