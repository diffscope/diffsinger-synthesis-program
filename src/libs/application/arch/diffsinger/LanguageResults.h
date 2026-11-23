#ifndef DSSP_ARCHDIFFSINGER_LANGUAGERESULTS_H
#define DSSP_ARCHDIFFSINGER_LANGUAGERESULTS_H

#include <wolf/Api/Inferences/G2p/1/G2pApiL1.h>

#include <dssparch/Errors.h>
#include <dssparch/Types.h>

namespace dssp::arch_diffsinger {
    arch::SynthesisResult<arch::Pronunciation> translatePronunciation(const wolf::Api::G2p::L1::WordOutput &, const std::string &lyric);
    std::vector<arch::Phoneme> slicePhonemes(std::vector<arch::Phoneme>, const arch::PronunciationNote &);
}

#endif // DSSP_ARCHDIFFSINGER_LANGUAGERESULTS_H
