#ifndef DSSP_ARCHDIFFSINGER_SINGERMETADATA_H
#define DSSP_ARCHDIFFSINGER_SINGERMETADATA_H

#include <expected>
#include <string>
#include <vector>

#include <synthrt/Support/DisplayText.h>
#include <synthrt/Support/JSON.h>

#include <dsspinference/InferenceError.h>

namespace dssp::arch_diffsinger {

    struct SpeakerMetadata {
        std::string id;
        srt::DisplayText name;
    };

    struct SingerMetadata {
        std::vector<SpeakerMetadata> speakers;
        bool permitsMixing = true;
    };

    std::expected<SingerMetadata, inference::ResourceError> singerMetadata(const srt::JsonValue &configuration);
}

#endif // DSSP_ARCHDIFFSINGER_SINGERMETADATA_H
