#include "ParallelismPolicy.h"

#include <algorithm>
#include <array>
#include <set>
#include <tuple>

#include <synthrt/Core/ContribImportBinding.h>

namespace dssp::inference {
    namespace {
        // These implementations do not invoke the ONNX driver. Exact matches
        // prevent a future implementation or contract level bypassing the gate.
        constexpr auto parallelLeaves = std::to_array<std::tuple<std::string_view, int, std::string_view>>({
            {"org.openvpi.wolf.inference.G2P", 1, "algo-pinyin"},
            {"org.openvpi.wolf.inference.S2P", 1, "direct"},
            {"org.openvpi.wolf.inference.S2P", 1, "dict"},
            {"org.openvpi.wolf.inference.S2P", 1, "mapping"},
            {"org.openvpi.wolf.inference.S2P", 1, "lua"},
            {"org.openvpi.wolf.inference.Onset", 1, "rule"},
            {"org.openvpi.wolf.inference.Onset", 1, "lua"}
        });

        bool inspect(const srt::ContribSpec &spec, std::set<const srt::ContribSpec *> &visiting) {
            const bool composite = spec.interfaceId() == "org.openvpi.wolf.inference.G2P" && spec.level() == 1 && spec.variant() == "pipe-chain";
            if (!composite && !isParallelLeaf(spec.interfaceId(), spec.level(), spec.variant())) return false;
            if (!visiting.insert(&spec).second) return false;
            for (const auto &imported : spec.imports()) {
                const auto *binding = imported.binding();
                if (!binding || binding->state() != srt::ContribImportBinding::Active || !inspect(binding->target(), visiting)) {
                    visiting.erase(&spec);
                    return false;
                }
            }
            visiting.erase(&spec);
            return true;
        }
    }

    bool isParallelLeaf(std::string_view interfaceId, int level, std::string_view variant) {
        return std::ranges::find(parallelLeaves, std::tuple{interfaceId, level, variant}) != parallelLeaves.end();
    }

    bool permitsParallelExecution(const srt::ContribSpec &spec) {
        std::set<const srt::ContribSpec *> visiting;
        return inspect(spec, visiting);
    }
}
