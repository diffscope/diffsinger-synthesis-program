#include "ServerConfig.h"

#include <charconv>
#include <cmath>
#include <concepts>
#include <exception>
#include <optional>
#include <system_error>
#include <tuple>

#include <absl/strings/charconv.h>
#include <stdcorelib/path.h>
#include <stdcorelib/support/systempaths.h>

namespace dssp::runtime {

    namespace {

        template <std::integral T>
        bool number(const std::string &text, T &value) {
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
            return error == std::errc{} && end == text.data() + text.size();
        }

        bool number(const std::string &text, double &value) {
            const auto [end, error] = absl::from_chars(text.data(), text.data() + text.size(), value);
            return error == std::errc{} && end == text.data() + text.size();
        }

        std::expected<std::filesystem::path, ConfigurationError> resolvePath(const char *option, const std::string &text) {
            if (text.empty()) {
                return std::unexpected{ConfigurationError(option, "A nonempty path must be configured")};
            }

            std::filesystem::path path;
            try {
                path = stdc::path::from_utf8(text);
            } catch (const std::exception &error) {
                return std::unexpected{ConfigurationError(option, "Cannot decode path", std::make_shared<ExternalError>(error.what(), NativeDiagnostic{"stdcorelib", "from_utf8", "exception"}))};
            }

            std::error_code error;
            auto absolute = std::filesystem::absolute(path, error);
            if (error) {
                return std::unexpected{ConfigurationError(option, "Cannot resolve absolute path", std::make_shared<ExternalError>(error.message(), NativeDiagnostic{"filesystem", "absolute", error.category().name(), error.value()}))};
            }

            return absolute.lexically_normal();
        }

    }

    std::expected<std::filesystem::path, ConfigurationError> defaultPackageDir() {
        using SystemPaths = stdc::system::SystemPaths;
        std::optional<std::filesystem::path> base;
        try {
#ifdef _WIN32
            // StateDirectory maps to LocalAppData on Windows.
            base = SystemPaths::writableDirectory(SystemPaths::StateDirectory);
#else
            base = SystemPaths::writableDirectory(SystemPaths::AppDataDirectory);
#endif
        } catch (const std::exception &error) {
            return std::unexpected{ConfigurationError("--package-dir", "Cannot resolve the default package directory", std::make_shared<ExternalError>(error.what(), NativeDiagnostic{"stdcorelib", "SystemPaths::writableDirectory", "exception"}))};
        }
        if (!base || base->empty() || !base->is_absolute()) {
            return std::unexpected{ConfigurationError("--package-dir", "Cannot determine the user data directory; configure --package-dir or DSSP_PACKAGE_DIR")};
        }

        return (*base / "OpenVPI" / "DiffScope_packages").lexically_normal();
    }

    std::expected<ServerConfig, ConfigurationError> resolveServerConfig(const ConfigValues &options, const ConfigValues &environment, const std::filesystem::path &applicationDir) {
        ServerConfig config;
        auto value = [&](const char *option, const char *env) -> std::optional<std::string> {
            if (const auto it = options.find(option); it != options.end()) {
                return it->second;
            }
            if (const auto it = environment.find(env); it != environment.end()) {
                return it->second;
            }
            return {};
        };

        if (const auto text = value("--host", "DSSP_HOST")) {
            config.host = *text;
        }
        if (config.host.empty()) {
            return std::unexpected{ConfigurationError("--host", "Listening host cannot be empty")};
        }

        if (const auto text = value("--port", "DSSP_PORT")) {
            unsigned port = 0;
            if (!number(*text, port) || port == 0 || port > 65535) {
                return std::unexpected{ConfigurationError("--port", "Port must be an integer in [1, 65535]")};
            }
            config.port = static_cast<std::uint16_t>(port);
        }

        if (const auto text = value("--max-request-body-size", "DSSP_MAX_REQUEST_BODY_SIZE")) {
            if (!number(*text, config.maxRequestBodySize) || config.maxRequestBodySize == 0) {
                return std::unexpected{ConfigurationError("--max-request-body-size", "Maximum request body size must be a positive integer in bytes representable by size_t")};
            }
        }

        const auto packages = value("--package-dir", "DSSP_PACKAGE_DIR");
        auto packagePath = packages ? resolvePath("--package-dir", *packages) : defaultPackageDir();
        if (!packagePath) {
            return std::unexpected{std::move(packagePath.error())};
        }

        std::error_code pathError;
        if (!std::filesystem::is_directory(*packagePath, pathError)) {
            Error::Ptr cause;
            if (pathError) {
                cause = std::make_shared<ExternalError>(pathError.message(), NativeDiagnostic{"filesystem", "is_directory", pathError.category().name(), pathError.value()});
            }
            return std::unexpected{ConfigurationError("--package-dir", "Package root must be an existing directory", std::move(cause))};
        }

        config.packageDir = std::move(*packagePath);
        if (const auto text = value("--execution-provider", "DSSP_EXECUTION_PROVIDER")) {
            auto provider = DeviceInfo::executionProviderFromString(*text);
            if (!provider) {
                return std::unexpected{ConfigurationError("--execution-provider", "Expected cpu, cuda, directml or coreml", provider.error().clone())};
            }
            config.executionProvider = *provider;
        }

        if (const auto text = value("--device-index", "DSSP_DEVICE_INDEX")) {
            if (!number(*text, config.deviceIndex) || config.deviceIndex < 0) {
                return std::unexpected{ConfigurationError("--device-index", "Device index must be a nonnegative integer")};
            }
        } else {
            auto device = DeviceInfo::defaultDevice(config.executionProvider);
            if (!device) {
                return std::unexpected{ConfigurationError("--device-index", "Cannot select a default device for the configured execution provider", device.error().clone())};
            }
            config.deviceIndex = device->index();
        }

        if (config.executionProvider == ExecutionProvider::CPU && config.deviceIndex != 0) {
            return std::unexpected{ConfigurationError("--device-index", "The CPU backend has only device index 0")};
        }

        if (const auto text = value("--silence-threshold", "DSSP_SILENCE_THRESHOLD")) {
            if (!number(*text, config.silenceThreshold) || !std::isfinite(config.silenceThreshold) || config.silenceThreshold < 0) {
                return std::unexpected{ConfigurationError("--silence-threshold", "Silence threshold must be a finite, nonnegative linear RMS value")};
            }
        }

        if (const auto text = value("--stream-heartbeat-interval", "DSSP_STREAM_HEARTBEAT_INTERVAL")) {
            std::int64_t seconds = 0;
            const auto maximumSeconds = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::duration::max() / 2).count();
            if (!number(*text, seconds) || seconds < 1 || seconds > maximumSeconds) {
                return std::unexpected{ConfigurationError("--stream-heartbeat-interval", "Stream heartbeat interval must be a positive integer in seconds within the monotonic timer limit")};
            }
            config.streamHeartbeatInterval = std::chrono::seconds(seconds);
        }

        for (const auto &descriptor : InferenceCacheGroups) {
            const auto group = descriptor.id;
            const auto prefix = descriptor.environmentPrefix;
            auto &cache = config.inferenceCaches.at(std::string(group));
            const auto capacityOption = "--" + std::string(group) + "-cache-capacity";
            const auto capacityEnvironment = std::string(prefix) + "_CACHE_CAPACITY";
            if (const auto text = value(capacityOption.c_str(), capacityEnvironment.c_str())) {
                if (!number(*text, cache.capacity)) {
                    return std::unexpected{ConfigurationError(capacityOption, "Cache capacity must be a nonnegative integer; zero disables the count limit")};
                }
            }

            for (const auto &[suffix, environmentSuffix, duration, allowZero] : {
                std::tuple{"-idle-timeout", "_IDLE_TIMEOUT", &cache.idleTimeout, true},
                std::tuple{"-cleanup-interval", "_CLEANUP_INTERVAL", &cache.cleanupInterval, false}
            }) {
                const auto option = "--" + std::string(group) + suffix;
                const auto environmentName = std::string(prefix) + environmentSuffix;
                if (const auto text = value(option.c_str(), environmentName.c_str())) {
                    std::int64_t seconds = 0;
                    const auto maximumSeconds = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::duration::max() / 2).count();
                    if (!number(*text, seconds) || seconds < (allowZero ? 0 : 1)) {
                        return std::unexpected{ConfigurationError(option, allowZero ? "Idle timeout must be a nonnegative integer in seconds; zero disables expiry" : "Cleanup interval must be a positive integer in seconds")};
                    }
                    if (seconds > maximumSeconds) {
                        return std::unexpected{ConfigurationError(option, "Cache duration exceeds the monotonic timer limit of " + std::to_string(maximumSeconds) + " seconds")};
                    }
                    *duration = std::chrono::milliseconds(seconds * 1000);
                }
            }
        }

#ifdef _WIN32
        constexpr const char *runtimeFile = "onnxruntime.dll";
#elif defined(__APPLE__)
        constexpr const char *runtimeFile = "libonnxruntime.dylib";
#else
        constexpr const char *runtimeFile = "libonnxruntime.so";
#endif
        if (!applicationDir.is_absolute()) {
            return std::unexpected{ConfigurationError("application-directory", "Deployment paths require an absolute application directory")};
        }

        config.resourceDir = (applicationDir / DSSP_DATA_RELATIVE_DIR).lexically_normal();
        config.pluginDir = (applicationDir / DSSP_PLUGIN_RELATIVE_DIR).lexically_normal();
        auto runtimeDirectory = config.pluginDir / "dsinfer" / "inferencedrivers" / "onnx" / "runtime";
        if (config.executionProvider == ExecutionProvider::CUDA) {
            runtimeDirectory /= "cuda";
        }

        config.runtimePath = runtimeDirectory / runtimeFile;
        // The inference environment checks provider/device availability and
        // deployed plugins. Configuration parsing never loads a runtime.
        return config;
    }

}
