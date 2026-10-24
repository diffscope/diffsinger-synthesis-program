#include "SingerMetadata.h"

#include <dsinfer/Api/Singers/DiffSinger/1/DiffSingerApiL1.h>

namespace dssp::arch_diffsinger {

    SingerMetadata singerMetadata(const ds::Api::DiffSinger::L1::DiffSingerConfiguration &) {
        // TODO: read the mixing permission when dsinfer exposes singer metadata.
        // The singer contract has no permission field. Mixing is permitted by default.
        return {};
    }
}
