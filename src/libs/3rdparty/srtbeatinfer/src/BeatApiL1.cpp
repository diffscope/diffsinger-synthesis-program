#include <cmath>
#include <limits>

#include <srtbeatinfer/Api/Beat/1/BeatApiL1.h>
#include <srtbeatinfer/Beat/Provider/BeatInterpreter.h>
#include <synthrt/Support/JSON.h>

namespace srtbeatinfer {
    srt::Expected<std::unique_ptr<srt::ContribImportOptions>> BeatInterpreter::createImportOptions(const srt::ContribSpec &target, const srt::JsonValue &options) const {
        if (!options.isNull() && (!options.isObject() || !options.toObject().empty()))
            return srt::Error(srt::Error::InvalidFormat, "Beat Level 1 defines no import options");

        return std::unique_ptr<srt::ContribImportOptions>(new Api::Beat::L1::BeatImportOptions(target.variant()));
    }
}

namespace srtbeatinfer::Api::Beat::L1 {
    BeatSchema::BeatSchema(std::string variant) : ContribExports(API_INTERFACE, std::move(variant), API_LEVEL) {}

    BeatSchema::~BeatSchema() = default;

    BeatExecutive::BeatExecutive(srt::InferenceSpec &spec) : InferenceExecutive(spec), m_task([this](const BeatStartInput &input) { return run(input); }) {}

    BeatExecutive::~BeatExecutive() {
        (void)stop();
        (void)waitForFinished();
    }

    srt::Expected<std::unique_ptr<BeatExecutive>> createBeatTracker(srt::InferenceSpec &spec) {
        if (spec.interfaceId() != API_INTERFACE || spec.level() != API_LEVEL)
            return srt::Error(BeatError::ContractViolation, "Expected a Beat Level 1 inference");

        auto made = spec.createInference(BeatImportOptions(spec.variant()), BeatRuntimeOptions(spec.variant()));
        if (!made) return made.takeError();

        auto executive = made.take();
        auto *typed = dynamic_cast<BeatExecutive *>(executive.get());
        if (!typed) return srt::Error(BeatError::ContractViolation, "Interpreter returned another executive contract");

        (void)executive.release();
        return std::unique_ptr<BeatExecutive>(typed);
    }

    srt::Expected<std::unique_ptr<BeatSchema>> readBeatSchema(const srt::ContribSpec &spec, std::string variant) {
        if (!spec.manifestExports().isObject()) return srt::Error(srt::Error::InvalidFormat, "Beat exports must be an object");

        const auto object = spec.manifestExports().toObject();
        for (const auto &[key, value] : object)
            if (key != "sampleRate" && key != "channelCount" && key != "maxSegmentDuration" && key != "downbeats")
                return srt::Error(srt::Error::InvalidFormat, "Unknown Beat exports field: " + key);

        auto result = std::make_unique<BeatSchema>(std::move(variant));
        for (const auto &[key, member] : {std::pair{"sampleRate", &result->sampleRate}, std::pair{"channelCount", &result->channelCount}}) {
            const auto it = object.find(key);
            if (it == object.end() || !it->second.isNumber()) return srt::Error(srt::Error::InvalidFormat, std::string(key) + " must be a positive integer");
            const auto value = it->second.toDouble();
            if (!std::isfinite(value) || value <= 0 || value > std::numeric_limits<int>::max() || std::floor(value) != value)
                return srt::Error(srt::Error::InvalidFormat, std::string(key) + " must be a positive integer");
            *member = static_cast<int>(value);
        }

        if (const auto it = object.find("maxSegmentDuration"); it != object.end()) {
            if (!it->second.isNumber()) return srt::Error(srt::Error::InvalidFormat, "maxSegmentDuration must be numeric");
            result->maxSegmentDuration = it->second.toDouble();
            if (!std::isfinite(result->maxSegmentDuration) || result->maxSegmentDuration < 0)
                return srt::Error(srt::Error::InvalidFormat, "maxSegmentDuration must be finite and nonnegative");
        }

        const auto it = object.find("downbeats");
        if (it == object.end() || !it->second.isBool()) return srt::Error(srt::Error::InvalidFormat, "downbeats must be a boolean");
        result->downbeats = it->second.toBool();

        return result;
    }
}
