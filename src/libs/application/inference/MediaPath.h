#ifndef DSSP_INFERENCE_MEDIAPATH_H
#define DSSP_INFERENCE_MEDIAPATH_H

#include <expected>

#include <synthrt/Support/DisplayText.h>
#include <synthrt/Support/Json.h>

#include <dsspinference/InferenceError.h>

namespace dssp::inference {
    // Keep native path resolution while restoring explicitly empty optional values.
    std::expected<srt::DisplayText, ResourceError> preserveEmptyMediaPaths(const srt::DisplayText &resolved, const srt::JsonValue &declaration);
}

#endif // DSSP_INFERENCE_MEDIAPATH_H
