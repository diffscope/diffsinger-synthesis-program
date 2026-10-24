#include "LanguageMetadata.h"

namespace dssp::inference {

    LanguageMetadata languageMetadata(const wolf::LinguistSpec &language) {
        return {language.name(), language.defaultLyric()};
    }
}
