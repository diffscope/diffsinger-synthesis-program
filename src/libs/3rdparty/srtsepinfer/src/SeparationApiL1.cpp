#include <cmath>
#include <limits>
#include <set>

#include <srtsepinfer/Api/Separation/1/SeparationApiL1.h>
#include <srtsepinfer/Separation/Provider/SeparationInterpreter.h>
#include <synthrt/Support/JSON.h>

namespace srtsepinfer {
    srt::Expected<std::unique_ptr<srt::ContribImportOptions>> SeparationInterpreter::createImportOptions(const srt::ContribSpec &target, const srt::JsonValue &options) const {
        if (!options.isNull() && (!options.isObject() || !options.toObject().empty()))
            return srt::Error(srt::Error::InvalidFormat, "Separation Level 1 defines no import options");

        return std::unique_ptr<srt::ContribImportOptions>(new Api::Separation::L1::SeparationImportOptions(target.variant()));
    }
}

namespace srtsepinfer::Api::Separation::L1 {
    SeparationSchema::SeparationSchema(std::string variant) : ContribExports(API_INTERFACE, std::move(variant), API_LEVEL) {}

    SeparationSchema::~SeparationSchema() = default;

    SeparationExecutive::SeparationExecutive(srt::InferenceSpec &spec) : InferenceExecutive(spec), m_task([this](const SeparationStartInput &input) { return run(input); }) {}

    SeparationExecutive::~SeparationExecutive() {
        (void)stop();
        (void)waitForFinished();
    }

    srt::Expected<std::unique_ptr<SeparationExecutive>> createSeparator(srt::InferenceSpec &spec) {
        if (spec.interfaceId() != API_INTERFACE || spec.level() != API_LEVEL)
            return srt::Error(SeparationError::ContractViolation, "Expected a Separation Level 1 inference");

        auto made = spec.createInference(SeparationImportOptions(spec.variant()), SeparationRuntimeOptions(spec.variant()));
        if (!made)
            return made.takeError();

        auto executive = made.take();
        auto *typed = dynamic_cast<SeparationExecutive *>(executive.get());
        if (!typed)
            return srt::Error(SeparationError::ContractViolation, "Interpreter returned an executive of another contract");

        (void)executive.release();
        return std::unique_ptr<SeparationExecutive>(typed);
    }

    srt::Expected<std::unique_ptr<SeparationSchema>> readSeparationSchema(const srt::ContribSpec &spec, std::string variant) {
        if (!spec.manifestExports().isObject())
            return srt::Error(srt::Error::InvalidFormat, "Separation exports must be an object");

        const auto object = spec.manifestExports().toObject();
        for (const auto &[key, value] : object) {
            if (key != "sampleRate" && key != "channelCount" && key != "maxSegmentDuration" && key != "tracks")
                return srt::Error(srt::Error::InvalidFormat, "Unknown Separation exports field: " + key);
        }

        auto result = std::make_unique<SeparationSchema>(std::move(variant));
        for (const auto &[key, member] : {std::pair{"sampleRate", &result->sampleRate}, std::pair{"channelCount", &result->channelCount}}) {
            const auto it = object.find(key);
            if (it == object.end() || !it->second.isNumber())
                return srt::Error(srt::Error::InvalidFormat, std::string(key) + " must be a positive integer");
            const auto value = it->second.toDouble();
            if (!std::isfinite(value) || value <= 0 || value > std::numeric_limits<int>::max() || std::floor(value) != value)
                return srt::Error(srt::Error::InvalidFormat, std::string(key) + " must be a positive integer");
            *member = static_cast<int>(value);
        }

        if (const auto it = object.find("maxSegmentDuration"); it != object.end()) {
            if (!it->second.isNumber())
                return srt::Error(srt::Error::InvalidFormat, "maxSegmentDuration must be a nonnegative number");
            result->maxSegmentDuration = it->second.toDouble();
            if (!std::isfinite(result->maxSegmentDuration) || result->maxSegmentDuration < 0)
                return srt::Error(srt::Error::InvalidFormat, "maxSegmentDuration must be finite and nonnegative");
        }

        const auto tracks = object.find("tracks");
        if (tracks == object.end() || !tracks->second.isArray() || tracks->second.toArray().empty())
            return srt::Error(srt::Error::InvalidFormat, "tracks must be a nonempty array");

        std::set<std::string> ids;
        for (const auto &value : tracks->second.toArray()) {
            if (!value.isObject())
                return srt::Error(srt::Error::InvalidFormat, "Each track must be an object");
            const auto track = value.toObject();
            if (track.size() != 2 || !track.count("id") || !track.count("description") || !track.at("id").isString())
                return srt::Error(srt::Error::InvalidFormat, "Each track needs only id and description");
            auto id = track.at("id").toString();
            if (id.empty() || !ids.insert(id).second)
                return srt::Error(srt::Error::InvalidFormat, "Track IDs must be nonempty and unique");

            const auto &text = track.at("description");
            srt::DisplayText description;
            if (text.isString() && !text.toString().empty()) {
                description = srt::DisplayText(text.toString());
            } else if (text.isObject()) {
                const auto translations = text.toObject();
                std::map<std::string, std::string> locales;
                for (const auto &[locale, translated] : translations) {
                    if (locale.empty() || !translated.isString() || translated.toString().empty())
                        return srt::Error(srt::Error::InvalidFormat, "Track description translations must be nonempty strings");
                    locales.emplace(locale, translated.toString());
                }

                const auto fallback = locales.find("_");
                if (fallback == locales.end())
                    return srt::Error(srt::Error::InvalidFormat, "A translated track description needs a '_' default");
                auto defaultText = fallback->second;
                locales.erase(fallback);
                description = srt::DisplayText(std::move(defaultText), locales);
            } else
                return srt::Error(srt::Error::InvalidFormat, "Track description must be a nonempty string or translation object");

            result->tracks.push_back({std::move(id), std::move(description)});
        }

        return result;
    }
}
