#ifndef DSSP_ARCHDIFFSINGER_SYNTHESISCONTEXT_H
#define DSSP_ARCHDIFFSINGER_SYNTHESISCONTEXT_H

#include <cstdint>
#include <string>
#include <vector>

#include <dssparchdiffsinger/SingerCatalog.h>

namespace dssp::arch_diffsinger {
    struct SingerSource {
        const SingerRecord *singer = nullptr;
        std::string speaker;
    };

    struct SynthesisSteps {
        std::int64_t pitch = 20;
        std::int64_t variance = 20;
        std::int64_t acoustic = 20;
    };

    struct SynthesisOptions {
        SynthesisSteps steps;
        float depth = 1;
    };

    struct SynthesisContext {
        SynthesisOptions options;
        std::vector<SingerSource> sources;
        const GroupRecord *group = nullptr;
    };

    arch::SynthesisResult<SynthesisOptions> synthesisOptions(const Json::Value &);
    arch::SynthesisResult<SingerSource> singerSource(const SingerCatalog &, const arch::Singer &);
    arch::SynthesisResult<SynthesisContext> synthesisContext(const SingerCatalog &, const arch::MultiSingerContext &);
}

#endif // DSSP_ARCHDIFFSINGER_SYNTHESISCONTEXT_H
