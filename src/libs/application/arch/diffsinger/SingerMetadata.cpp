#include "SingerMetadata.h"

#include <map>
#include <set>
#include <utility>

namespace dssp::arch_diffsinger {
    namespace {
        std::expected<srt::DisplayText, inference::ResourceError> speakerName(const srt::JsonValue &value) {
            if (value.isString()) {
                return srt::DisplayText(value.toString());
            }
            if (!value.isObject()) {
                return std::unexpected{inference::ResourceError("Speaker name must be a string or a language map")};
            }

            const auto &declaration = value.toObject();
            const auto fallback = declaration.find("_");
            if (fallback == declaration.end() || !fallback->second.isString()) {
                return std::unexpected{inference::ResourceError("Speaker name language map requires a string _ field")};
            }

            std::map<std::string, std::string> translations;
            for (const auto &[locale, text] : declaration) {
                if (!text.isString()) {
                    return std::unexpected{inference::ResourceError("Speaker name language map must contain string values")};
                }
                if (locale != "_") {
                    translations.emplace(locale, text.toString());
                }
            }
            return srt::DisplayText(fallback->second.toString(), translations);
        }
    }

    std::expected<SingerMetadata, inference::ResourceError> singerMetadata(const srt::JsonValue &configuration) {
        SingerMetadata result;
        const auto &declaration = configuration.toObject();
        const auto speakers = declaration.find("speakers");
        if (speakers == declaration.end()) {
            return result;
        }
        if (!speakers->second.isArray()) {
            return std::unexpected{inference::ResourceError("Singer configuration.speakers must be an array")};
        }

        std::set<std::string> ids;
        for (const auto &entry : speakers->second.toArray()) {
            if (!entry.isObject()) {
                return std::unexpected{inference::ResourceError("Singer configuration.speakers must contain objects")};
            }
            const auto &speaker = entry.toObject();
            const auto idField = speaker.find("id");
            const auto nameField = speaker.find("name");
            if (idField == speaker.end() || !idField->second.isString() || idField->second.toString().empty()) {
                return std::unexpected{inference::ResourceError("Declared speaker requires a nonempty string id")};
            }
            auto id = idField->second.toString();
            if (!ids.insert(id).second) {
                return std::unexpected{inference::ResourceError("Duplicate declared speaker id: " + id)};
            }
            if (nameField == speaker.end()) {
                return std::unexpected{inference::ResourceError("Declared speaker requires a name: " + id)};
            }
            auto name = speakerName(nameField->second);
            if (!name) {
                return std::unexpected{inference::ResourceError("Invalid name for speaker " + id, name.error().clone())};
            }
            result.speakers.push_back({std::move(id), std::move(*name)});
        }
        return result;
    }
}
