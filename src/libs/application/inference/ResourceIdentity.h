#ifndef DSSP_INFERENCE_RESOURCEIDENTITY_H
#define DSSP_INFERENCE_RESOURCEIDENTITY_H

#include <expected>
#include <map>
#include <string>
#include <string_view>

#include <synthrt/Core/ContribSpec.h>

#include <dsspinference/InferenceError.h>

namespace dssp::inference {
    // Fully qualified public reference: package=version:category/contribution.
    std::string resourceReference(const srt::ContribSpec &);
    // Internal graph key; its encoding is retained to keep group fingerprints stable.
    std::string resourceId(const srt::ContribSpec &);
    std::expected<std::string, ResourceError> fingerprint(std::string_view);
    // Includes resolved target versions and recursive imports. Singer-level
    // speaker mappings select sources within a shared model and are excluded.
    srt::JsonValue describeImports(const srt::ContribSpec &, const std::map<std::string, srt::JsonValue> &effectiveOptions = {}, const std::map<std::string, std::string> &roleNames = {});
}

#endif // DSSP_INFERENCE_RESOURCEIDENTITY_H
