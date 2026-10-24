#include "ResourceIdentity.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <span>

#include <openssl/err.h>
#include <openssl/evp.h>
#include <stdcorelib/path.h>

#include <synthrt/Core/ContribImportBinding.h>
#include <synthrt/Core/ContribLocator.h>

#include <dsspcodecutil/codecutil.h>

namespace dssp::inference {
    namespace {
        void appendField(std::string &text, std::string_view field) {
            text += std::to_string(field.size());
            text += ':';
            text += field;
        }

        // Expanded ${root} paths describe package resources, not installation
        // identity. Normalize them without opening files or interpreting models.
        srt::JsonValue resourcePaths(const srt::JsonValue &value, const srt::ContribSpec &spec) {
            if (value.isString()) {
                const auto path = stdc::path::from_utf8(value.toString());
                if (path.is_absolute()) {
                    const auto relative = path.lexically_normal().lexically_relative(spec.package().path());
                    if (!relative.empty() && *relative.begin() != "..") {
                        const auto text = relative.generic_u8string();
                        return srt::JsonObject{{"packageResource", std::string(text.begin(), text.end())}};
                    }
                }
            } else if (value.isArray()) {
                auto array = value.toArray();
                for (auto &item : array) {
                    item = resourcePaths(item, spec);
                }

                return array;
            } else if (value.isObject()) {
                auto object = value.toObject();
                for (auto &[key, item] : object) {
                    item = resourcePaths(item, spec);
                }

                return object;
            }

            return value;
        }

        srt::JsonArray importsOf(const srt::ContribSpec &spec, srt::JsonObject &nodes, bool singer, const std::map<std::string, srt::JsonValue> &effectiveOptions = {}, const std::map<std::string, std::string> &roleNames = {}) {
            srt::JsonArray imports;
            for (const auto &entry : spec.imports()) {
                if (!entry.binding()) {
                    continue;
                }

                const auto &target = entry.binding()->target();
                const auto id = resourceId(target);
                srt::JsonValue options = entry.manifestOptions();
                if (const auto it = effectiveOptions.find(entry.role()); it != effectiveOptions.end()) {
                    options = it->second;
                }
                if (singer && entry.role().starts_with("singer/") && options.isObject()) {
                    auto value = options.toObject();
                    value.erase("speakerMapping");
                    options = std::move(value);
                }

                const auto renamed = roleNames.find(entry.role());
                imports.push_back(srt::JsonObject{{"role", renamed == roleNames.end() ? entry.role() : renamed->second}, {"target", id}, {"options", std::move(options)}});
                if (!nodes.contains(id)) {
                    // Insert first to represent shared/cyclic edges by identity.
                    nodes.emplace(id, srt::JsonValue{});
                    nodes[id] = srt::JsonObject{
                        {"interface", target.interfaceId()}, {"variant", target.variant()}, {"level", target.level()},
                        {"configuration", resourcePaths(target.manifestConfiguration(), target)}, {"exports", resourcePaths(target.manifestExports(), target)},
                        {"imports", importsOf(target, nodes, false)}
                    };
                }
            }

            std::ranges::sort(imports, [](const auto &left, const auto &right) { return left.toObject().find("role")->second.toString() < right.toObject().find("role")->second.toString(); });
            return imports;
        }
    }

    std::string resourceReference(const srt::ContribSpec &spec) {
        const auto package = spec.package();
        // The installed ContribLocator formats the category/contribution suffix
        // but carries no version. Pin the owning package separately in the reference.
        const srt::ContribLocator local(spec.locator().category(), spec.locator().contributionId());
        return package.id() + '=' + package.version().toString() + local.toString();
    }

    std::string resourceId(const srt::ContribSpec &spec) {
        const auto package = spec.package();
        std::string identity;
        appendField(identity, package.id());
        appendField(identity, package.version().toString());
        appendField(identity, spec.locator().category());
        appendField(identity, spec.locator().contributionId());
        const codecutil::Base64EncodingView<codecutil::Base64Mode::URL> encoded(identity);
        return "r1." + std::string(encoded.begin(), encoded.end());
    }

    std::expected<std::string, ResourceError> fingerprint(std::string_view text) {
        std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
        unsigned length = 0;
        if (EVP_Digest(text.data(), text.size(), digest.data(), &length, EVP_sha256(), nullptr) != 1) {
            const auto code = ERR_get_error();
            std::array<char, 256> message{};
            ERR_error_string_n(code, message.data(), message.size());
            return std::unexpected{ResourceError("Cannot compute the resource fingerprint", std::make_shared<const ExternalError>(message.data(), NativeDiagnostic{"OpenSSL", "EVP_Digest(SHA256)", "OpenSSL", static_cast<std::int64_t>(code)}))};
        }

        const codecutil::HexEncodingView encoded(std::span(digest.data(), length));
        return std::string(encoded.begin(), encoded.end());
    }

    srt::JsonValue describeImports(const srt::ContribSpec &spec, const std::map<std::string, srt::JsonValue> &effectiveOptions, const std::map<std::string, std::string> &roleNames) {
        srt::JsonObject nodes;
        auto imports = importsOf(spec, nodes, true, effectiveOptions, roleNames);
        return srt::JsonObject{{"imports", std::move(imports)}, {"nodes", std::move(nodes)}};
    }
}
