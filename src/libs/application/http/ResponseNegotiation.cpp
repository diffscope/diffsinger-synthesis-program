#include "ResponseNegotiation.h"

#include <cstddef>
#include <string>
#include <system_error>
#include <vector>

#include <absl/strings/charconv.h>

namespace dssp::http {
    namespace {
        std::string_view trim(std::string_view text) {
            const auto first = text.find_first_not_of(" \t");
            if (first == text.npos) {
                return {};
            }
            return text.substr(first, text.find_last_not_of(" \t") - first + 1);
        }

        std::string lowercase(std::string_view text) {
            std::string result(text);
            for (auto &c : result) {
                if (c >= 'A' && c <= 'Z') {
                    c += 'a' - 'A';
                }
            }
            return result;
        }

        std::vector<std::string_view> split(std::string_view text, char delimiter) {
            std::vector<std::string_view> result;
            bool quoted = false;
            bool escaped = false;
            std::size_t begin = 0;
            for (std::size_t i = 0; i < text.size(); ++i) {
                if (escaped) {
                    escaped = false;
                } else if (quoted && text[i] == '\\') {
                    escaped = true;
                } else if (text[i] == '"') {
                    quoted = !quoted;
                } else if (!quoted && text[i] == delimiter) {
                    result.push_back(trim(text.substr(begin, i - begin)));
                    begin = i + 1;
                }
            }
            result.push_back(trim(text.substr(begin)));
            return result;
        }

        struct Preference {
            int specificity = -1;
            double quality = 0;
        };
    }

    TaskRepresentation negotiateTaskResponse(std::string_view accept) {
        if (trim(accept).empty()) {
            return TaskRepresentation::Json;
        }
        Preference json, ndjson;
        for (const auto range : split(accept, ',')) {
            const auto parts = split(range, ';');
            const auto type = lowercase(parts.front());
            double quality = 1;
            bool weighted = false;
            bool valid = true;
            int parameters = 0;
            for (std::size_t i = 1; i < parts.size(); ++i) {
                const auto equal = parts[i].find('=');
                const auto name = lowercase(trim(parts[i].substr(0, equal)));
                if (weighted) {
                    continue;
                }
                if (equal == parts[i].npos) {
                    valid = false;
                    break;
                }
                auto value = trim(parts[i].substr(equal + 1));
                if (name == "q") {
                    weighted = true;
                    const auto [end, error] = absl::from_chars(value.data(), value.data() + value.size(), quality);
                    if (error != std::errc{} || end != value.data() + value.size() || !(quality >= 0 && quality <= 1)) {
                        valid = false;
                    }
                } else {
                    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
                        value = value.substr(1, value.size() - 2);
                    }
                    if (name != "charset" || lowercase(value) != "utf-8") {
                        valid = false;
                    }
                    ++parameters;
                }
            }
            if (!valid) {
                continue;
            }
            const auto consider = [&](std::string_view mediaType, Preference &preference) {
                const int specificity = type == mediaType ? 2 : type == "application/*" ? 1 : type == "*/*" ? 0 : -1;
                if (specificity < 0) {
                    return;
                }
                const int score = specificity * 2 + (parameters > 0 ? 1 : 0);
                if (score > preference.specificity || (score == preference.specificity && quality > preference.quality)) {
                    preference = {score, quality};
                }
            };
            consider("application/json", json);
            consider("application/x-ndjson", ndjson);
        }
        if (json.quality == 0 && ndjson.quality == 0) {
            return TaskRepresentation::Unacceptable;
        }
        if (ndjson.quality > json.quality || (ndjson.quality == json.quality && ndjson.specificity > json.specificity)) {
            return TaskRepresentation::Ndjson;
        }
        return TaskRepresentation::Json;
    }
}
