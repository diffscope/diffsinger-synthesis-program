#ifndef DSSP_API_V1_CODEC_REQUESTCODEC_H
#define DSSP_API_V1_CODEC_REQUESTCODEC_H

#include <dsspapiv1/Requests.h>
#include <dssphttp/JsonPayload.h>
#include <dssplocalization/Context.h>

namespace dssp::api_v1 {
    template <class T>
    std::expected<T, http::DtoErrors> fromJson(const Json::Value &, const localization::Context &locale = {}) = delete;
    template <>
    std::expected<dto::EnvTagRequest, http::DtoErrors> fromJson<dto::EnvTagRequest>(const Json::Value &, const localization::Context &);
    template <>
    std::expected<dto::PronunciationRequest, http::DtoErrors> fromJson<dto::PronunciationRequest>(const Json::Value &, const localization::Context &);
    template <>
    std::expected<dto::PhonemeRequest, http::DtoErrors> fromJson<dto::PhonemeRequest>(const Json::Value &, const localization::Context &);
    template <>
    std::expected<dto::DurationRequest, http::DtoErrors> fromJson<dto::DurationRequest>(const Json::Value &, const localization::Context &);
    template <>
    std::expected<dto::ParameterRequest, http::DtoErrors> fromJson<dto::ParameterRequest>(const Json::Value &, const localization::Context &);
    template <>
    std::expected<dto::AudioRequest, http::DtoErrors> fromJson<dto::AudioRequest>(const Json::Value &, const localization::Context &);
    template <>
    std::expected<dto::ExtractionRequest, http::DtoErrors> fromJson<dto::ExtractionRequest>(const Json::Value &, const localization::Context &);
    template <>
    std::expected<dto::SeparationExtractionRequest, http::DtoErrors> fromJson<dto::SeparationExtractionRequest>(const Json::Value &, const localization::Context &);

    std::expected<dto::MediaQuery, http::DtoErrors> mediaQuery(const drogon::HttpRequest &request, const localization::Context &locale = {});

    template <class T>
    using Decoded = std::expected<T, http::RequestErrors>;
    template <class T>
    Decoded<T> fromHttp(const drogon::HttpRequest &request, const localization::Context &locale) {
        auto json = http::readJsonPayload(request, locale);
        if (!json) {
            return std::unexpected{widen_errors<http::RequestErrors>(std::move(json.error()))};
        }

        auto dto = fromJson<T>(*json, locale);
        if (!dto) {
            return std::unexpected{widen_errors<http::RequestErrors>(std::move(dto.error()))};
        }

        return std::move(*dto);
    }
}

#endif // DSSP_API_V1_CODEC_REQUESTCODEC_H
