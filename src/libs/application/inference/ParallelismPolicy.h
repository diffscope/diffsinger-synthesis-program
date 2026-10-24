#ifndef DSSP_INFERENCE_PARALLELISMPOLICY_H
#define DSSP_INFERENCE_PARALLELISMPOLICY_H

#include <string_view>

#include <synthrt/Core/ContribSpec.h>

namespace dssp::inference {
    // This is a host whitelist, not an inference/provider concurrency contract.
    // Unknown interfaces, levels, variants, unresolved imports and cycles require
    // serialization. Updating providers never implicitly enlarges the whitelist.
    bool isParallelLeaf(std::string_view interfaceId, int level, std::string_view variant);
    bool permitsParallelExecution(const srt::ContribSpec &);
}

#endif // DSSP_INFERENCE_PARALLELISMPOLICY_H
