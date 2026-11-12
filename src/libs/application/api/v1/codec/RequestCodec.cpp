#include "RequestCodec.h"

#include <cmath>
#include <utility>

#include <drogon/utils/Utilities.h>
#include <stdcorelib/str.h>

#include <dssphttp/MediaType.h>
#include <dssphttp/Uri.h>

namespace dssp::api_v1 {
    namespace {
        using http::DtoErrors;
        using http::FieldIssue;
        using http::ValidationError;

        std::string escapePointer(std::string_view name) {
            std::string result;
            for (auto c : name) {
                result += c == '~' ? "~0" : c == '/' ? "~1"
                                                     : std::string(1, c);
            }
            return result;
        }

        // Small typed reader, not a second JSON parser or architecture validator.
        // Type checks precede every JSONcpp accessor, which would otherwise throw.
        struct Node {
            const Json::Value *value;
            std::string path;
            std::vector<FieldIssue> &issues;
            std::optional<http::HttpBoundaryError> &boundaryError;
            const localization::Context &locale;
            void issue(std::string type, std::string reason) const { issues.push_back({path, std::move(type), std::move(reason)}); }

            bool object() const {
                if (!value) {
                    return false;
                }
                if (value->isObject()) {
                    return true;
                }
                issue("type", locale.text("api_v1.field_object_expected", "Expected an object"));
                return false;
            }

            Node field(std::string_view name, bool required = true) const {
                const auto childPath = path + "/" + escapePointer(name);
                if (!value || !value->isObject()) {
                    return {nullptr, childPath, issues, boundaryError, locale};
                }
                const std::string key(name);
                if (!value->isMember(key)) {
                    if (required) {
                        issues.push_back({childPath, "required", locale.text("api_v1.field_required", "Required field is missing")});
                    }
                    return {nullptr, childPath, issues, boundaryError, locale};
                }
                return {&(*value)[key], childPath, issues, boundaryError, locale};
            }

            Node element(Json::ArrayIndex i) const { return {&(*value)[i], path + "/" + std::to_string(i), issues, boundaryError, locale}; }

            std::string string() const {
                if (!value) {
                    return {};
                }
                if (!value->isString()) {
                    issue("type", locale.text("api_v1.field_string_expected", "Expected a string"));
                    return {};
                }
                return value->asString();
            }

            bool boolean() const {
                if (!value) {
                    return false;
                }
                if (!value->isBool()) {
                    issue("type", locale.text("api_v1.field_boolean_expected", "Expected a boolean"));
                    return false;
                }
                return value->asBool();
            }

            double number(std::optional<double> minimum = {}, bool exclusive = false, std::optional<double> maximum = {}) const {
                if (!value) {
                    return 0;
                }
                if (!value->isNumeric()) {
                    issue("type", locale.text("api_v1.field_number_expected", "Expected a number"));
                    return 0;
                }
                const auto result = value->asDouble();
                if (!std::isfinite(result)) {
                    issue("finite", locale.text("api_v1.field_finite_number_expected", "Expected a finite number"));
                } else if (minimum && (exclusive ? result <= *minimum : result < *minimum)) {
                    issue(exclusive ? "exclusiveMinimum" : "minimum", locale.text("api_v1.field_number_below_range", "Number is below the allowed range"));
                } else if (maximum && result > *maximum) {
                    issue("maximum", locale.text("api_v1.field_number_above_range", "Number exceeds the allowed range"));
                }
                return result;
            }

            std::uint64_t unsignedInteger(std::optional<std::uint64_t> maximum = {}) const {
                if (!value) {
                    return 0;
                }
                if (!value->isNumeric() || !std::isfinite(value->asDouble()) || std::trunc(value->asDouble()) != value->asDouble()) {
                    issue("type", locale.text("api_v1.field_integer_expected", "Expected an integer"));
                    return 0;
                }
                if (!value->isUInt64()) {
                    issue("minimum", locale.text("api_v1.field_unsigned_integer_expected", "Expected a nonnegative, representable integer"));
                    return 0;
                }
                const auto result = value->asUInt64();
                if (maximum && result > *maximum) {
                    issue("maximum", locale.text("api_v1.field_integer_above_range", "Integer exceeds the allowed range"));
                }
                return result;
            }

            std::optional<std::int64_t> nullableInteger() const {
                if (!value || value->isNull()) {
                    return {};
                }
                if (!value->isInt64()) {
                    issue("type", locale.text("api_v1.field_nullable_integer_expected", "Expected an integer or null within int64 range"));
                    return {};
                }
                return value->asInt64();
            }

            bool array(std::size_t minimum = 0) const {
                if (!value) {
                    return false;
                }
                if (!value->isArray()) {
                    issue("type", locale.text("api_v1.field_array_expected", "Expected an array"));
                    return false;
                }
                if (value->size() < minimum) {
                    issue("minItems", locale.text("api_v1.field_array_too_short", "Array contains too few items"));
                }
                return true;
            }

            Json::Value json() const { return value ? *value : Json::Value{}; }
        };

        template <class T, class Read>
        std::vector<T> array(const Node &node, Read read, std::size_t minimum = 0) {
            std::vector<T> result;
            if (node.array(minimum)) {
                result.reserve(node.value->size());
                for (Json::ArrayIndex i = 0; i < node.value->size(); ++i) {
                    result.push_back(read(node.element(i)));
                }
            }
            return result;
        }

        dto::Singer singer(const Node &n) {
            dto::Singer result;
            if (n.object()) {
                result.id = n.field("id").string();
                result.extra = n.field("extra").json();
            }
            return result;
        }

        dto::SingleSingerContext singleContext(const Node &n) {
            dto::SingleSingerContext result;
            if (n.object()) {
                result.arch = n.field("arch").string();
                result.archExtra = n.field("archExtra").json();
                result.singer = singer(n.field("singer"));
            }
            return result;
        }

        dto::MultiSingerContext multiContext(const Node &n) {
            dto::MultiSingerContext result;
            if (n.object()) {
                result.arch = n.field("arch").string();
                result.archExtra = n.field("archExtra").json();
                result.singers = array<dto::Singer>(n.field("singers"), singer, 1);
            }
            return result;
        }

        dto::NotePosition position(const Node &n) {
            dto::NotePosition result;
            if (n.object()) {
                result.gap = n.field("gap").number(0);
                result.duration = n.field("duration").number(0);
            }
            return result;
        }

        template <class Phone>
        Phone phone(const Node &n) {
            Phone result;
            if (n.object()) {
                result.token = n.field("token").string();
                result.onset = n.field("onset").boolean();
                result.language = n.field("language").string();
                if constexpr (std::same_as<Phone, dto::ParameterInputPhoneme>) {
                    result.start = n.field("start").number();
                }
            }
            return result;
        }

        template <class Phone>
        dto::Note<Phone> note(const Node &n) {
            dto::Note<Phone> result;
            if (n.object()) {
                result.position = position(n.field("position"));
                result.cent = static_cast<std::uint32_t>(n.field("cent").unsignedInteger(12800));
                result.pronunciation = n.field("pronunciation").string();
                result.language = n.field("language").string();
                result.phonemes = array<Phone>(n.field("phonemes"), phone<Phone>);
                const auto kind = n.field("kind");
                const auto text = kind.string();
                if (text == "slur") {
                    result.kind = dto::NoteKind::Slur;
                } else if (kind.value && kind.value->isString() && text != "normal") {
                    kind.issue("enum", kind.locale.text("api_v1.field_note_kind_expected", "Expected normal or slur"));
                }
            }
            return result;
        }

        template <class Phone, class Input>
        void piece(const Node &n, Input &result) {
            if (!n.object()) {
                return;
            }
            result.pieceDuration = n.field("pieceDuration").number(0);
            result.mixSampleRate = n.field("mixSampleRate").number(0, true);
            result.notes = array<dto::Note<Phone>>(n.field("notes"), note<Phone>);
            result.mix = array<std::vector<double>>(n.field("mix"), [](const Node &row) { return array<double>(row, [](const Node &v) { return v.number(0, false, 1); }); });
        }

        dto::ParameterRetake retake(const Node &n) {
            dto::ParameterRetake result;
            if (n.object()) {
                result.position = n.field("position").unsignedInteger();
                result.length = n.field("length").unsignedInteger();
            }
            return result;
        }

        template <class Parameter>
        Parameter parameter(const Node &n) {
            Parameter result;
            if (n.object()) {
                result.sampleRate = n.field("sampleRate").number(0, true);
                const auto values = n.field("values", std::same_as<Parameter, dto::Parameter>);
                if constexpr (std::same_as<Parameter, dto::Parameter>) {
                    result.values = array<double>(values, [](const Node &v) { return v.number(); });
                    const auto r = n.field("retake", false);
                    if (r.value) {
                        result.retake = retake(r);
                    }
                } else if (values.value) {
                    result.values = array<double>(values, [](const Node &v) { return v.number(); });
                }
            }
            return result;
        }

        template <class Parameter>
        std::map<std::string, Parameter> parameters(const Node &n) {
            std::map<std::string, Parameter> result;
            if (n.object()) {
                for (const auto &key : n.value->getMemberNames()) {
                    result.emplace(key, parameter<Parameter>(n.field(key)));
                }
            }
            return result;
        }

        std::vector<std::string> strings(const Node &n, bool mediaTypes) {
            return array<std::string>(n, [mediaTypes](const Node &item) {
                auto text = item.string();
                if (mediaTypes && item.value && item.value->isString()) {
                    auto matched = http::matchesMediaTypeName(text);
                    if (!matched) {
                        if (!item.boundaryError) {
                            item.boundaryError = std::move(matched.error());
                        }
                    } else if (!*matched) {
                        item.issue("format", item.locale.text("api_v1.field_mime_type_expected", "Expected MIME type/subtype without parameters or wildcards"));
                    }
                }
                return text;
            });
        }

        template <class Request>
        void acceptableMedia(const Node &n, Request &result) {
            const auto formats = n.field("acceptableFormats", false), schemes = n.field("acceptableSchemes", false);
            if (formats.value) {
                result.acceptableFormats = strings(formats, true);
            }
            if (schemes.value) {
                result.acceptableSchemes = strings(schemes, false);
            }
        }

        void extractionRequest(const Node &n, dto::ExtractionRequest &result) {
            result.extractor = n.field("extractor").string();
            const auto input = n.field("input");
            if (input.object()) {
                const auto url = input.field("audioUrl");
                result.input.audioUrl = url.string();
                if (url.value && url.value->isString()) {
                    auto checked = http::validateUri(result.input.audioUrl, n.locale);
                    if (!checked) {
                        if (const auto *invalid = checked.error().get_if<http::UriSyntaxError>()) {
                            url.issue("format", invalid->message());
                        } else if (const auto *boundary = checked.error().get_if<http::HttpBoundaryError>()) {
                            url.boundaryError = *boundary;
                        }
                    }
                }
            }
        }

        template <class T, class Read>
        std::expected<T, DtoErrors> decode(const Json::Value &json, const localization::Context &locale, Read read) {
            std::vector<FieldIssue> issues;
            std::optional<http::HttpBoundaryError> boundaryError;
            Node root{&json, "", issues, boundaryError, locale};
            T result{};
            if (root.object()) {
                read(root, result);
            }

            if (boundaryError) {
                return fail<DtoErrors>(std::move(*boundaryError));
            }
            if (!issues.empty()) {
                return fail<DtoErrors>(ValidationError(std::move(issues)));
            }

            return result;
        }
    }

    template <>
    std::expected<dto::EnvTagRequest, DtoErrors> fromJson<dto::EnvTagRequest>(const Json::Value &json, const localization::Context &locale) {
        return decode<dto::EnvTagRequest>(json, locale, [](const Node &n, auto &r) { r.context = multiContext(n.field("context")); });
    }

    template <>
    std::expected<dto::PronunciationRequest, DtoErrors> fromJson<dto::PronunciationRequest>(const Json::Value &json, const localization::Context &locale) {
        return decode<dto::PronunciationRequest>(json, locale, [](const Node &n, auto &r) {
            r.context = singleContext(n.field("context"));
            const auto input = n.field("input");
            if (input.object()) {
                r.input.notes = array<dto::Lyric>(input.field("notes"), [](const Node &v) {
                    dto::Lyric result;
                    if (v.object()) {
                        result.lyric = v.field("lyric").string();
                        result.language = v.field("language").string();
                    }
                    return result;
                });
            }
        });
    }

    template <>
    std::expected<dto::PhonemeRequest, DtoErrors> fromJson<dto::PhonemeRequest>(const Json::Value &json, const localization::Context &locale) {
        return decode<dto::PhonemeRequest>(json, locale, [](const Node &n, auto &r) {
            r.context = singleContext(n.field("context"));
            const auto input = n.field("input");
            if (input.object()) {
                r.input.notes = array<dto::PronunciationNote>(input.field("notes"), [](const Node &v) {
                    dto::PronunciationNote result;
                    if (v.object()) {
                        result.pronunciation = v.field("pronunciation").string();
                        result.language = v.field("language").string();
                        result.syllableSliceStart = v.field("syllableSliceStart", false).nullableInteger();
                        result.syllableSliceEnd = v.field("syllableSliceEnd", false).nullableInteger();
                    }
                    return result;
                });
            }
        });
    }

    template <>
    std::expected<dto::DurationRequest, DtoErrors> fromJson<dto::DurationRequest>(const Json::Value &json, const localization::Context &locale) {
        return decode<dto::DurationRequest>(json, locale, [](const Node &n, auto &r) {
            r.context = multiContext(n.field("context"));
            piece<dto::DurationInputPhoneme>(n.field("input"), r.input);
        });
    }

    template <>
    std::expected<dto::ParameterRequest, DtoErrors> fromJson<dto::ParameterRequest>(const Json::Value &json, const localization::Context &locale) {
        return decode<dto::ParameterRequest>(json, locale, [](const Node &n, auto &r) {
            r.context = multiContext(n.field("context"));
            const auto input = n.field("input");
            piece<dto::ParameterInputPhoneme>(input, r.input);
            if (input.value && input.value->isObject()) {
                r.input.parameters = parameters<dto::Parameter>(input.field("parameters"));
            }
        });
    }

    template <>
    std::expected<dto::AudioRequest, DtoErrors> fromJson<dto::AudioRequest>(const Json::Value &json, const localization::Context &locale) {
        return decode<dto::AudioRequest>(json, locale, [](const Node &n, auto &r) {
            r.context = multiContext(n.field("context"));
            const auto input = n.field("input");
            piece<dto::ParameterInputPhoneme>(input, r.input);
            if (input.value && input.value->isObject()) {
                r.input.parameters = parameters<dto::AudioParameter>(input.field("parameters"));
            }
            acceptableMedia(n, r);
        });
    }

    template <>
    std::expected<dto::ExtractionRequest, DtoErrors> fromJson<dto::ExtractionRequest>(const Json::Value &json, const localization::Context &locale) {
        return decode<dto::ExtractionRequest>(json, locale, extractionRequest);
    }

    template <>
    std::expected<dto::SeparationExtractionRequest, DtoErrors> fromJson<dto::SeparationExtractionRequest>(const Json::Value &json, const localization::Context &locale) {
        return decode<dto::SeparationExtractionRequest>(json, locale, [](const Node &n, auto &r) {
            extractionRequest(n, r);
            acceptableMedia(n, r);
        });
    }

    std::expected<dto::MediaQuery, DtoErrors> mediaQuery(const drogon::HttpRequest &request, const localization::Context &locale) {
        dto::MediaQuery result;
        std::vector<FieldIssue> issues;
        auto parameter = [&](const char *name) -> std::pair<std::string_view, bool> {
            // In-memory requests can have parameters set without a query string.
            if (request.query().empty()) {
                return {request.getParameter(name), false};
            }

            std::string_view value;
            for (const auto part : stdc::str::split(request.query(), "&")) {
                const auto equals = part.find('=');
                if (drogon::utils::urlDecode(part.substr(0, equals)) == name) {
                    value = equals == std::string_view::npos ? std::string_view{} : part.substr(equals + 1);
                }
            }

            return {value, true};
        };

        auto read = [&](const char *name, std::vector<std::string> &output, bool mime) -> std::expected<void, http::HttpBoundaryError> {
            const auto [value, encoded] = parameter(name);
            const auto text = stdc::str::trim(value, " \t");
            if (text.empty()) {
                return {};
            }

            std::size_t i = 0;
            // OpenAPI form/explode:false arrays split before percent-decoding;
            // an encoded comma belongs to one item rather than separating two.
            for (const auto part : stdc::str::split(text, ",")) {
                const auto decoded = encoded ? drogon::utils::urlDecode(part) : std::string(part);
                const auto item = stdc::str::trim(decoded, " \t");
                output.emplace_back(item);
                if (mime) {
                    auto matched = http::matchesMediaTypeName(item);
                    if (!matched) {
                        return std::unexpected{std::move(matched.error())};
                    }
                    if (!*matched) {
                        issues.push_back({std::string("/") + name + "/" + std::to_string(i), "format", locale.text("api_v1.field_mime_type_expected", "Expected MIME type/subtype without parameters or wildcards")});
                    }
                }
                ++i;
            }

            return {};
        };

        if (auto formats = read("acceptableFormats", result.acceptableFormats, true); !formats) {
            return fail<DtoErrors>(std::move(formats.error()));
        }
        if (auto schemes = read("acceptableSchemes", result.acceptableSchemes, false); !schemes) {
            return fail<DtoErrors>(std::move(schemes.error()));
        }

        if (!issues.empty()) {
            return fail<DtoErrors>(ValidationError(std::move(issues)));
        }

        return result;
    }

}
