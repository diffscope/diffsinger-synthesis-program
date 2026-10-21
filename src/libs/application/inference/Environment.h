#ifndef DSSP_INFERENCE_ENVIRONMENT_H
#define DSSP_INFERENCE_ENVIRONMENT_H

#include <expected>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <synthrt/Core/PackageHandle.h>

#include <dsspinference/InferenceError.h>
#include <dsspinference/InferencePool.h>
#include <dsspruntime/DeviceInfo.h>

namespace srt { class SynthUnit; }

namespace dssp::inference {

    struct PluginSearchPath {
        std::string category;
        std::filesystem::path path;
    };

    struct OnnxOptions {
        std::filesystem::path driverPath;
        std::filesystem::path runtimePath;
        runtime::ExecutionProvider executionProvider = runtime::ExecutionProvider::CPU;
        int deviceIndex = 0;
    };

    struct EnvironmentOptions {
        std::vector<std::filesystem::path> packageRoots;
        // The subset of packageRoots protected by dspm shared store locks.
        std::vector<std::filesystem::path> packageStoreRoots;
        std::vector<PluginSearchPath> pluginPaths;
        // Empty for a metadata-only host or a host registering another runtime service.
        std::optional<OnnxOptions> onnx;
        std::map<std::string, resource::CacheOptions> cacheGroups;
    };

    struct PackageProblem {
        std::filesystem::path path;
        ResourceError error;
    };

    // Startup commits the package set once. Architecture catalogues retain this
    // owner rather than keeping borrowed specs after their SynthUnit is gone.
    class Environment final {
    public:
        static std::expected<std::shared_ptr<Environment>, InitializationError> create(const EnvironmentOptions &);
        ~Environment();
        Environment(const Environment &) = delete;
        Environment &operator=(const Environment &) = delete;

        srt::SynthUnit &unit() const noexcept;
        std::expected<InferencePool *, InitializationError> inferences(std::string_view group);
        void shutdown();
        const std::vector<srt::PackageHandle> &packages() const noexcept;
        // Returns the indexed content hash, or nullptr for an unregistered package.
        const std::string *packageHash(const srt::PackageHandle &) const;
        const std::vector<PackageProblem> &problems() const noexcept;

    private:
        class Impl;
        explicit Environment(std::unique_ptr<Impl>);
        std::unique_ptr<Impl> m_impl;
    };

}

#endif // DSSP_INFERENCE_ENVIRONMENT_H
