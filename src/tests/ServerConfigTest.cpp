#include <chrono>
#include <limits>
#include <map>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <stdcorelib/path.h>
#include <stdcorelib/support/systempaths.h>

#include <dsspruntime/ServerConfig.h>

namespace dssp::tests {

    namespace {
        // Configuration tests unrelated to device selection must not depend on host GPUs.
        auto resolveCpuConfig(const runtime::ConfigValues &options, runtime::ConfigValues environment, const std::filesystem::path &applicationDir) {
            environment.try_emplace("DSSP_EXECUTION_PROVIDER", "cpu");
            return runtime::resolveServerConfig(options, environment, applicationDir);
        }
    }

    TEST(ServerConfig, CliOverridesEnvironmentAndDefaults) {
        const auto root = std::filesystem::current_path();
        const runtime::ConfigValues options{{"--package-dir", root.string()}, {"--port", "20000"}, {"--silence-threshold", "0"}};
        const runtime::ConfigValues environment{{"DSSP_PORT", "invalid"}, {"DSSP_HOST", "0.0.0.0"}, {"DSSP_PACKAGE_DIR", ""}};
        auto result = resolveCpuConfig(options, environment, root);
        ASSERT_TRUE(result);
        EXPECT_EQ(result->port, 20000);
        EXPECT_EQ(result->host, "0.0.0.0");
        EXPECT_EQ(result->silenceThreshold, 0);
        EXPECT_EQ(result->deviceIndex, 0);
        EXPECT_TRUE(result->packageDir.is_absolute());
        EXPECT_TRUE(result->runtimePath.is_absolute());
    }

    TEST(ServerConfig, SilenceThresholdAcceptsDecimalAndScientificNotation) {
        const auto root = std::filesystem::current_path();
        for (const auto &[text, expected] : {std::pair{"0", 0.0}, {"-0", -0.0}, {"0.125", 0.125}, {".5", 0.5}, {"1.", 1.0}, {"2e-3", 0.002}, {"1e+3", 1000.0}, {"1e-300", 1e-300}, {"4.9406564584124654e-324", std::numeric_limits<double>::denorm_min()}}) {
            const auto commandLine = resolveCpuConfig({{"--package-dir", root.string()}, {"--silence-threshold", text}}, {}, root);
            ASSERT_TRUE(commandLine) << text;
            EXPECT_DOUBLE_EQ(commandLine->silenceThreshold, expected) << text;
            const auto environment = resolveCpuConfig({{"--package-dir", root.string()}}, {{"DSSP_SILENCE_THRESHOLD", text}}, root);
            ASSERT_TRUE(environment) << text;
            EXPECT_DOUBLE_EQ(environment->silenceThreshold, expected) << text;
        }
    }

    TEST(ServerConfig, SilenceThresholdRejectsInvalidNonfiniteAndOutOfRangeValues) {
        const auto root = std::filesystem::current_path();
        for (const auto text : {"", "-0.1", "nan", "inf", "-inf", "+0.5", " 0.5", "0.5 ", "0,5", "0x1p-1", "0X1P-1", "-0x0p0", "1e309", "1e-9999", "0.5junk", "1e"}) {
            const auto commandLine = resolveCpuConfig({{"--package-dir", root.string()}, {"--silence-threshold", text}}, {}, root);
            ASSERT_FALSE(commandLine) << text;
            EXPECT_EQ(commandLine.error().option(), "--silence-threshold");
            const auto environment = resolveCpuConfig({{"--package-dir", root.string()}}, {{"DSSP_SILENCE_THRESHOLD", text}}, root);
            ASSERT_FALSE(environment) << text;
            EXPECT_EQ(environment.error().option(), "--silence-threshold");
        }
    }

    TEST(ServerConfig, InferenceCacheGroupsHaveIndependentDefaultsAndCliOverridesEnvironmentPerField) {
        const auto root = std::filesystem::current_path();
        const auto defaults = resolveCpuConfig({{"--package-dir", root.string()}}, {}, root);
        ASSERT_TRUE(defaults);
        for (const auto &descriptor : runtime::InferenceCacheGroups) {
            const auto group = std::string(descriptor.id);
            const auto &cache = defaults->inferenceCaches.at(group);
            EXPECT_EQ(cache.capacity, 16U);
            EXPECT_EQ(cache.idleTimeout, std::chrono::seconds(600));
            EXPECT_EQ(cache.cleanupInterval, std::chrono::seconds(60));
        }

        const auto configured = resolveCpuConfig({{"--package-dir", root.string()}, {"--diffsinger-pronunciation-cache-capacity", "3"}, {"--diffsinger-acoustic-idle-timeout", "0"}}, {{"DSSP_DIFFSINGER_PRONUNCIATION_CACHE_CAPACITY", "bad"}, {"DSSP_DIFFSINGER_PRONUNCIATION_IDLE_TIMEOUT", "12"}, {"DSSP_DIFFSINGER_ACOUSTIC_CACHE_CAPACITY", "0"}, {"DSSP_DIFFSINGER_ACOUSTIC_CLEANUP_INTERVAL", "7"}}, root);
        ASSERT_TRUE(configured);
        EXPECT_EQ(configured->inferenceCaches.at("diffsinger-pronunciation").capacity, 3U);
        EXPECT_EQ(configured->inferenceCaches.at("diffsinger-pronunciation").idleTimeout, std::chrono::seconds(12));
        EXPECT_EQ(configured->inferenceCaches.at("diffsinger-pronunciation").cleanupInterval, std::chrono::seconds(60));
        EXPECT_EQ(configured->inferenceCaches.at("diffsinger-acoustic").capacity, 0U);
        EXPECT_EQ(configured->inferenceCaches.at("diffsinger-acoustic").idleTimeout, std::chrono::seconds(0));
        EXPECT_EQ(configured->inferenceCaches.at("diffsinger-acoustic").cleanupInterval, std::chrono::seconds(7));
        const auto extraction = resolveCpuConfig({{"--package-dir", root.string()}, {"--extraction-note-cache-capacity", "2"}, {"--extraction-pitch-idle-timeout", "9"}}, {}, root);
        ASSERT_TRUE(extraction);
        EXPECT_EQ(extraction->inferenceCaches.at("extraction-note").capacity, 2U);
        EXPECT_EQ(extraction->inferenceCaches.at("extraction-pitch").capacity, 16U);
        EXPECT_EQ(extraction->inferenceCaches.at("extraction-note").idleTimeout, std::chrono::seconds(600));
        EXPECT_EQ(extraction->inferenceCaches.at("extraction-pitch").idleTimeout, std::chrono::seconds(9));
    }

    TEST(ServerConfig, StreamHeartbeatDefaultsAndOverrides) {
        const auto root = std::filesystem::current_path();
        const runtime::ConfigValues packages{{"--package-dir", root.string()}};
        const auto defaults = resolveCpuConfig(packages, {}, root);
        ASSERT_TRUE(defaults);
        EXPECT_EQ(defaults->streamHeartbeatInterval, std::chrono::seconds(10));
        const auto environment = resolveCpuConfig(packages, {{"DSSP_STREAM_HEARTBEAT_INTERVAL", "3"}}, root);
        ASSERT_TRUE(environment);
        EXPECT_EQ(environment->streamHeartbeatInterval, std::chrono::seconds(3));
        const auto commandLine = resolveCpuConfig({{"--package-dir", root.string()}, {"--stream-heartbeat-interval", "7"}}, {{"DSSP_STREAM_HEARTBEAT_INTERVAL", "invalid"}}, root);
        ASSERT_TRUE(commandLine);
        EXPECT_EQ(commandLine->streamHeartbeatInterval, std::chrono::seconds(7));
    }

    TEST(ServerConfig, StreamHeartbeatRejectsInvalidIntervals) {
        const auto root = std::filesystem::current_path();
        for (const auto value : {"", "0", "-1", "1.5", "10s", " 10", "99999999999999999999"}) {
            const auto commandLine = resolveCpuConfig({{"--package-dir", root.string()}, {"--stream-heartbeat-interval", value}}, {}, root);
            ASSERT_FALSE(commandLine) << value;
            EXPECT_EQ(commandLine.error().option(), "--stream-heartbeat-interval");
            const auto environment = resolveCpuConfig({{"--package-dir", root.string()}}, {{"DSSP_STREAM_HEARTBEAT_INTERVAL", value}}, root);
            ASSERT_FALSE(environment) << value;
            EXPECT_EQ(environment.error().option(), "--stream-heartbeat-interval");
        }
    }

    TEST(ServerConfig, RequestBodyLimitDefaultsToOneGiBAndCliOverridesEnvironment) {
        const auto root = std::filesystem::current_path();
        const runtime::ConfigValues packages{{"--package-dir", root.string()}};
        const auto defaults = resolveCpuConfig(packages, {}, root);
        ASSERT_TRUE(defaults);
        EXPECT_EQ(defaults->maxRequestBodySize, 1073741824U);

        const auto environment = resolveCpuConfig(packages, {{"DSSP_MAX_REQUEST_BODY_SIZE", "2097152"}}, root);
        ASSERT_TRUE(environment);
        EXPECT_EQ(environment->maxRequestBodySize, 2097152U);

        const auto commandLine = resolveCpuConfig({{"--package-dir", root.string()}, {"--max-request-body-size", "2147483648"}}, {{"DSSP_MAX_REQUEST_BODY_SIZE", "invalid"}}, root);
        ASSERT_TRUE(commandLine);
        EXPECT_EQ(commandLine->maxRequestBodySize, 2147483648ULL);

        const auto maximum = resolveCpuConfig({{"--package-dir", root.string()}, {"--max-request-body-size", std::to_string(std::numeric_limits<std::size_t>::max())}}, {}, root);
        ASSERT_TRUE(maximum);
        EXPECT_EQ(maximum->maxRequestBodySize, std::numeric_limits<std::size_t>::max());
    }

    TEST(ServerConfig, RequestBodyLimitRejectsInvalidValuesFromCliAndEnvironment) {
        const auto root = std::filesystem::current_path();
        for (const auto value : {"", "0", "-1", "1.5", "1GiB", "1024suffix", " 1024", "999999999999999999999999"}) {
            const auto commandLine = resolveCpuConfig({{"--package-dir", root.string()}, {"--max-request-body-size", value}}, {}, root);
            ASSERT_FALSE(commandLine) << value;
            EXPECT_EQ(commandLine.error().option(), "--max-request-body-size");
            const auto environment = resolveCpuConfig({{"--package-dir", root.string()}}, {{"DSSP_MAX_REQUEST_BODY_SIZE", value}}, root);
            ASSERT_FALSE(environment) << value;
            EXPECT_EQ(environment.error().option(), "--max-request-body-size");
        }
    }

    TEST(ServerConfig, InferenceCacheParametersRejectNegativeFractionalOverflowAndZeroScanIntervals) {
        const auto root = std::filesystem::current_path();
        for (const auto &descriptor : runtime::InferenceCacheGroups) {
            const auto group = std::string(descriptor.id);
            for (const auto &[suffix, invalidValues] : std::map<std::string, std::vector<std::string>>{
                {"-cache-capacity", {"-1", "1.5", "999999999999999999999"}},
                {"-idle-timeout", {"-1", "1.5", "9223372036854775807"}},
                {"-cleanup-interval", {"0", "-1", "1.5"}}
            }) {
                const auto option = "--" + std::string(group) + suffix;
                for (const auto &value : invalidValues) {
                    const auto result = resolveCpuConfig({{"--package-dir", root.string()}, {option, value}}, {}, root);
                    ASSERT_FALSE(result) << option << '=' << value;
                    EXPECT_EQ(result.error().option(), option);
                }
            }
        }
    }

    TEST(ServerConfig, DefaultPackageRootUsesThePlatformUserDataLayout) {
        using SystemPaths = stdc::system::SystemPaths;
#ifdef _WIN32
        const auto base = SystemPaths::writableDirectory(SystemPaths::StateDirectory);
#else
        const auto base = SystemPaths::writableDirectory(SystemPaths::AppDataDirectory);
#endif
        const auto result = runtime::defaultPackageDir();
        if (!base) {
            ASSERT_FALSE(result);
            EXPECT_EQ(result.error().option(), "--package-dir");
            return;
        }

        ASSERT_TRUE(result);
        EXPECT_EQ(*result, (*base / "OpenVPI" / "DiffScope_packages").lexically_normal());
        EXPECT_TRUE(result->is_absolute());
    }

    TEST(ServerConfig, DeploymentPathsUseGnuLayoutAndFollowTheExecutableLocation) {
        const auto root = std::filesystem::current_path();
        for (const auto name : {"original-installation", "moved-installation"}) {
            const auto prefix = root / name;
            const auto result = resolveCpuConfig({{"--package-dir", stdc::path::to_utf8(root)}}, {}, prefix / "bin");
            ASSERT_TRUE(result);
            EXPECT_EQ(result->pluginDir, prefix / "lib" / "plugins");
            EXPECT_EQ(result->resourceDir, prefix / "share");
            EXPECT_EQ(result->runtimePath.parent_path(), result->pluginDir / "dsinfer" / "inferencedrivers" / "onnx" / "runtime");
        }
    }

    TEST(ServerConfig, BundledDeploymentPathsIgnoreEnvironmentOverrides) {
        const auto root = std::filesystem::current_path();
        const auto applicationDir = root / "deployment" / "bin";
        const runtime::ConfigValues environment{{"DSSP_PLUGIN_DIR", stdc::path::to_utf8(root / "external-plugins")}, {"DSSP_RUNTIME_PATH", stdc::path::to_utf8(root / "external-runtime")}};
        const runtime::ConfigValues options{{"--package-dir", stdc::path::to_utf8(root)}};
        const auto bundled = resolveCpuConfig(options, {}, applicationDir);
        ASSERT_TRUE(bundled);
        const auto result = resolveCpuConfig(options, environment, applicationDir);
        ASSERT_TRUE(result);
        EXPECT_EQ(result->pluginDir, bundled->pluginDir);
        EXPECT_EQ(result->runtimePath, bundled->runtimePath);
        EXPECT_EQ(result->resourceDir, bundled->resourceDir);

        const auto relativeApplication = resolveCpuConfig(options, environment, "bin");
        ASSERT_FALSE(relativeApplication);
        EXPECT_EQ(relativeApplication.error().option(), "application-directory");
    }

    TEST(ServerConfig, OmittedPackageRootResolvesAndValidatesTheSystemDefault) {
        const auto root = std::filesystem::current_path();
        const auto packages = runtime::defaultPackageDir();
        const auto result = resolveCpuConfig({}, {}, root);
        if (!packages) {
            ASSERT_FALSE(result);
            EXPECT_EQ(result.error().option(), "--package-dir");
            EXPECT_EQ(result.error().message(), packages.error().message());
            return;
        }

        // Read the user's directory without creating or modifying it.
        std::error_code error;
        if (!std::filesystem::is_directory(*packages, error)) {
            ASSERT_FALSE(result);
            EXPECT_EQ(result.error().option(), "--package-dir");
            EXPECT_EQ(result.error().message(), "Package root must be an existing directory");
            return;
        }

        ASSERT_TRUE(result);
        EXPECT_EQ(result->packageDir, *packages);
    }

    TEST(ServerConfig, EnvironmentPackageRootOverridesTheSystemDefault) {
        const auto root = std::filesystem::current_path();
        const auto result = resolveCpuConfig({}, {{"DSSP_PACKAGE_DIR", stdc::path::to_utf8(root)}}, root);
        ASSERT_TRUE(result);
        EXPECT_EQ(result->packageDir, root.lexically_normal());
    }

    TEST(ServerConfig, EmptyPackageRootCannotBecomeTheWorkingDirectory) {
        const auto root = std::filesystem::current_path();
        EXPECT_FALSE(resolveCpuConfig({{"--package-dir", ""}}, {{"DSSP_PACKAGE_DIR", root.string()}}, root));
        const auto environment = resolveCpuConfig({}, {{"DSSP_PACKAGE_DIR", ""}}, root);
        ASSERT_FALSE(environment);
        EXPECT_EQ(environment.error().option(), "--package-dir");
        EXPECT_EQ(environment.error().message(), "A nonempty path must be configured");
    }

    TEST(ServerConfig, MalformedNumbersReturnErrorsWithoutThrowing) {
        const auto root = std::filesystem::current_path();
        for (const auto port : {"0", "65536", "-1", "123suffix", "99999999999999999999"}) {
            const auto result = resolveCpuConfig({{"--package-dir", root.string()}, {"--port", port}}, {}, root);
            ASSERT_FALSE(result);
            EXPECT_EQ(result.error().option(), "--port");
        }

        for (const auto threshold : {"nan", "inf", "-0.1", "0.02suffix"}) {
            EXPECT_FALSE(resolveCpuConfig({{"--package-dir", root.string()}, {"--silence-threshold", threshold}}, {}, root));
        }
    }

    TEST(ServerConfig, BackendChoiceAndNativeDeviceIndexArePreserved) {
        const auto root = std::filesystem::current_path();
        auto result = resolveCpuConfig({{"--package-dir", root.string()}, {"--execution-provider", "cuda"}, {"--device-index", "7"}}, {}, root);
        ASSERT_TRUE(result);
        EXPECT_EQ(result->executionProvider, runtime::ExecutionProvider::CUDA);
        EXPECT_EQ(result->deviceIndex, 7);
    }

    TEST(ServerConfig, InvalidBackendAndCpuDeviceReturnTypedErrors) {
        const auto root = std::filesystem::current_path();
        const auto backend = resolveCpuConfig({{"--package-dir", root.string()}, {"--execution-provider", "unknown"}}, {}, root);
        ASSERT_FALSE(backend);
        EXPECT_EQ(backend.error().option(), "--execution-provider");
        ASSERT_NE(backend.error().cause(), nullptr);
        EXPECT_EQ(backend.error().cause()->kind(), "dssp.device");
        EXPECT_FALSE(resolveCpuConfig({{"--package-dir", root.string()}, {"--device-index", "1"}}, {}, root));
    }

    TEST(ServerConfig, OmittedDeviceMatchesTheInventoryDefault) {
        const auto root = std::filesystem::current_path();
        const auto device = runtime::DeviceInfo::defaultDevice();
        const auto result = runtime::resolveServerConfig({{"--package-dir", stdc::path::to_utf8(root)}}, {}, root);
        if (!device) {
            ASSERT_FALSE(result);
            EXPECT_EQ(result.error().option(), "--device-index");
            ASSERT_NE(result.error().cause(), nullptr);
            EXPECT_EQ(result.error().cause()->kind(), "dssp.device");
            return;
        }

        ASSERT_TRUE(result);
        EXPECT_EQ(result->executionProvider, device->executionProvider());
        EXPECT_EQ(result->deviceIndex, device->index());
    }

    TEST(ServerConfig, ExplicitCpuAndEnvironmentDeviceOverridesArePreserved) {
        const auto root = std::filesystem::current_path();
        const runtime::ConfigValues environment{{"DSSP_EXECUTION_PROVIDER", "cuda"}, {"DSSP_DEVICE_INDEX", "7"}};
        auto result = runtime::resolveServerConfig({{"--package-dir", stdc::path::to_utf8(root)}}, environment, root);
        ASSERT_TRUE(result);
        EXPECT_EQ(result->executionProvider, runtime::ExecutionProvider::CUDA);
        EXPECT_EQ(result->deviceIndex, 7);
        EXPECT_EQ(result->runtimePath.parent_path().filename(), "cuda");

        result = runtime::resolveServerConfig({{"--package-dir", stdc::path::to_utf8(root)}, {"--execution-provider", "cpu"}, {"--device-index", "0"}}, environment, root);
        ASSERT_TRUE(result);
        EXPECT_EQ(result->executionProvider, runtime::ExecutionProvider::CPU);
        EXPECT_EQ(result->deviceIndex, 0);

        result = runtime::resolveServerConfig({{"--package-dir", stdc::path::to_utf8(root)}, {"--execution-provider", "cpu"}}, {}, root);
        ASSERT_TRUE(result);
        EXPECT_EQ(result->executionProvider, runtime::ExecutionProvider::CPU);
        EXPECT_EQ(result->deviceIndex, 0);
    }

    TEST(ServerConfig, ExplicitIndexDoesNotChangeTheDefaultProvider) {
        const auto root = std::filesystem::current_path();
        const auto result = runtime::resolveServerConfig({{"--package-dir", stdc::path::to_utf8(root)}, {"--device-index", "0"}}, {}, root);
        ASSERT_TRUE(result);
        EXPECT_EQ(result->executionProvider, runtime::DeviceInfo::defaultExecutionProvider());
        EXPECT_EQ(result->deviceIndex, 0);
    }

    TEST(ServerConfig, UnavailableAutomaticDeviceReturnsAConfigurationError) {
        const auto root = std::filesystem::current_path();
        const auto result = runtime::resolveServerConfig({{"--package-dir", stdc::path::to_utf8(root)}, {"--execution-provider", "coreml"}}, {}, root);
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().option(), "--device-index");
        ASSERT_NE(result.error().cause(), nullptr);
        EXPECT_EQ(result.error().cause()->kind(), "dssp.device");
    }

}
