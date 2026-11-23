#ifndef DSSP_TESTS_TESTLANGUAGEPACKAGE_H
#define DSSP_TESTS_TESTLANGUAGEPACKAGE_H

#include <string>

#include <dssptests/TestFiles.h>

namespace dssp::tests {
    struct LanguagePackageOptions {
        std::string version = "1";
        bool singer = false;
        bool onsets = true;
        bool dictionaryS2p = false;
        bool missingS2pDictionary = false;
        bool singerLanguages = true;
        bool customReserved = false;
        bool synthesisStages = false;
        bool noReserved = false;
    };
    // Real Wolf modules with deterministic, local resources. Singer model
    // declarations deliberately point to missing files: discovery must stay cold.
    inline void writeLanguagePackage(const TestFiles &files, const std::filesystem::path &root, const LanguagePackageOptions &options = {}) {
        const std::string onsetDeclaration = options.onsets ? R"(,{"id":"onset","path":"onset.json"})" : "";
        const std::string predictedInferences = options.synthesisStages ? R"(,{"id":"duration","path":"duration.json"},{"id":"pitch","path":"pitch.json"},{"id":"variance","path":"variance.json"})" : "";
        const std::string singerInferences = options.singer ? R"(,{"id":"acoustic","path":"acoustic.json"},{"id":"vocoder","path":"vocoder.json"})" + predictedInferences : "";
        const std::string singerDeclaration = options.singer ? R"(,"singer":[{"id":"main","path":"singer.json"}])" : "";
        files.write(root / "desc.json", R"({"$version":"1.0","id":"example/language","version":")" + options.version + R"(","runtimeLevel":1,"contributions":{
            "inference":[{"id":"g2p","path":"g2p.json"},{"id":"s2p","path":"s2p.json"})" +
                                            onsetDeclaration + singerInferences + R"(],
            "linguist":[{"id":"eng","path":"eng.json"}])" +
                                            singerDeclaration + "}}");
        files.write(root / "g2p.json", R"({"interface":"org.openvpi.wolf.inference.G2P","variant":"pipe-chain","level":1,
            "exports":{"languages":[{"language":"eng","scheme":"test"}],"symbols":["b","a","i"]},
            "configuration":{"formatVersion":1,"steps":[{"step":"dict","params":{"file":"g2p.tsv"}}]}})");
        files.write(root / "g2p.tsv", "read\tb a b i\nread(2)\tb i\na+\ta+\n-\t-\n");
        const bool dictionary = options.dictionaryS2p || options.missingS2pDictionary;
        files.write(root / "s2p.json", R"({"interface":"org.openvpi.wolf.inference.S2P","variant":")" + std::string(dictionary ? "dict" : "direct") + R"(","level":1,
            "exports":{"languages":[{"language":"eng","scheme":"test"}],"phonemes":["b","a","i"]},"configuration":)" +
                                           (dictionary ? R"({"file":"s2p.tsv"})" : "{}") + "}");
        if (!options.missingS2pDictionary) {
            files.write(root / "s2p.tsv", "test\tb a b i\n");
        }
        files.write(root / "onset.json", R"({"interface":"org.openvpi.wolf.inference.Onset","variant":"rule","level":1,
            "exports":{"knownPhonemes":["b","a","i"]},"configuration":{"file":"onsets.json"}})");
        files.write(root / "onsets.json", R"({"phonemeTypes":{"b":"consonant","a":"vowel","i":"vowel"},
            "rules":[{"pattern":["consonant","vowel"],"onsets":[0]},{"pattern":["vowel"],"onsets":[0]}]})");
        const std::string onsetImport = options.onsets ? R"(,{"role":"linguist/onset","ref":":inference/onset","options":{}})" : "";
        files.write(root / "eng.json", R"({"interface":"org.openvpi.wolf.linguist.WolfLinguist","variant":"wolf","level":1,
            "language":"eng","scheme":"test","name":"English","exports":{"phonemes":["b","a","i"]},"configuration":{},
            "imports":[{"role":"linguist/g2p","ref":":inference/g2p","options":{}},{"role":"linguist/s2p","ref":":inference/s2p","options":{}})" +
                                           onsetImport + "]}");
        if (!options.singer) {
            return;
        }
        files.write(root / "acoustic.json", R"({"interface":"org.openvpi.dsinfer.inference.Acoustic","variant":"onnx","level":1,
            "exports":{"varianceControls":["energy"],"transitionControls":["gender","velocity"]},
            "configuration":{"model":"${root}/missing-acoustic.onnx","phonemes":"phonemes.json","parameters":["energy","gender","velocity"]}})");
        files.write(root / "vocoder.json", R"({"interface":"org.openvpi.dsinfer.inference.Vocoder","variant":"onnx","level":1,
            "exports":{},"configuration":{"model":"${root}/missing-vocoder.onnx","pitchControllable":true}})");
        files.write(root / "phonemes.json", R"({"b":0,"a":1,"i":2,"SP":3,"AP":4,"REST":5})");
        if (options.synthesisStages) {
            files.write(root / "duration.json", R"({"interface":"org.openvpi.dsinfer.inference.Duration","variant":"onnx","level":1,
                "exports":{},"configuration":{"encoder":"${root}/missing-duration-encoder.onnx","predictor":"${root}/missing-duration-predictor.onnx","frameWidth":0.01,"phonemes":"phonemes.json"}})");
            files.write(root / "pitch.json", R"({"interface":"org.openvpi.dsinfer.inference.Pitch","variant":"onnx","level":1,
                "exports":{"allowExpressiveness":true},"configuration":{"encoder":"${root}/missing-pitch-encoder.onnx","predictor":"${root}/missing-pitch-predictor.onnx","frameWidth":0.01,"phonemes":"phonemes.json","useExpressiveness":true}})");
            files.write(root / "variance.json", R"({"interface":"org.openvpi.dsinfer.inference.Variance","variant":"onnx","level":1,
                "exports":{"predictions":["energy"]},"configuration":{"encoder":"${root}/missing-variance-encoder.onnx","predictor":"${root}/missing-variance-predictor.onnx","frameWidth":0.01,"phonemes":"phonemes.json"}})");
        }
        // The DiffSinger provider requires a default language whenever the singer declares languages.
        const std::string languages = options.singerLanguages ? R"("languages":{"eng":"language/eng"},"defaultLanguage":"eng")" : "";
        const std::string languageImport = options.singerLanguages ? R"(,{"role":"language/eng","ref":":linguist/eng","options":{}})" : "";
        const std::string reserved = options.noReserved ? "" : options.customReserved ? R"("reservedPhonemes":["REST"])"
                                                                                      : R"("reservedPhonemes":["SP","AP"])";
        const std::string exports = R"("exports":{)" + std::string(languages.empty() ? "" : languages) + std::string(!languages.empty() && !reserved.empty() ? "," : "") + reserved + "}";
        const std::string predictedImports = options.synthesisStages ? R"(,{"role":"singer/duration","ref":":inference/duration","options":{}},{"role":"singer/pitch","ref":":inference/pitch","options":{}},{"role":"singer/variance","ref":":inference/variance","options":{"predictions":["energy"]}})" : "";
        files.write(root / "singer.json", R"({"interface":"org.openvpi.dsinfer.singer.DiffSinger","variant":"openvpi","level":1,
            "name":"Test singer","configuration":{"dict":"unused-dictionary.txt"},)" +
                                              exports + R"(,"imports":[
            {"role":"singer/acoustic","ref":":inference/acoustic","options":{}},{"role":"singer/vocoder","ref":":inference/vocoder","options":{}})" +
                                              languageImport + predictedImports + "]}");
    }
}

#endif // DSSP_TESTS_TESTLANGUAGEPACKAGE_H
