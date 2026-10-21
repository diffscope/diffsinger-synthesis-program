#include "Environment.h"

#include <algorithm>
#include <exception>
#include <map>
#include <mutex>
#include <set>
#include <utility>

// Parse interface() declarations before provider SDK headers introduce Windows macros.
#include <synthrt/Core/SynthUnit.h>

#include <onnxruntime_c_api.h>
#if defined(_WIN32) && __has_include(<dml_provider_factory.h>)
#    include <dml_provider_factory.h>
#    define DSSP_INFERENCE_HAS_DML
#endif

#include <dsinfer/Api/Drivers/Onnx/OnnxDriverApi.h>
#include <dsinfer/Inference/InferenceDriverFactory.h>
#include <stdcorelib/path.h>
#include <stdcorelib/scope_guard.h>
#include <stdcorelib/support/logging.h>
#include <stdcorelib/support/sharedlibrary.h>
#include <wolf/Linguist/LinguistContrib.h>

#include <dsspinference/NativeError.h>
#include <dsspinference/PackageStoreLock.h>

namespace dssp::inference {
    namespace {
        stdc::LogCategory log("dssp.inference.Environment");
        std::mutex libraryPathMutex;

        Error::Ptr ortError(const OrtApi &api, OrtStatus *status, std::string operation) {
            const auto release = stdc::make_scope_guard([&] { api.ReleaseStatus(status); });
            return std::make_shared<const ExternalError>(api.GetErrorMessage(status), NativeDiagnostic{"ONNX Runtime", std::move(operation), "OrtErrorCode", api.GetErrorCode(status)});
        }

        ds::Api::Onnx::ExecutionProvider providerFor(runtime::ExecutionProvider provider) {
            switch (provider) {
                case runtime::ExecutionProvider::CPU: return ds::Api::Onnx::ExecutionProvider::CPU;
                case runtime::ExecutionProvider::CUDA: return ds::Api::Onnx::ExecutionProvider::CUDA;
                case runtime::ExecutionProvider::DirectML: return ds::Api::Onnx::ExecutionProvider::DML;
                case runtime::ExecutionProvider::CoreML: return ds::Api::Onnx::ExecutionProvider::CoreML;
            }
            std::unreachable();
        }

        std::expected<void, InitializationError> checkProvider(const ds::Api::Onnx::DriverExtension &extension, const OnnxOptions &options) {
            if (!extension.runtimeApi.ortApi || extension.runtimeApi.ortApiVersion != ORT_API_VERSION) {
                return std::unexpected{InitializationError("The ONNX driver exposes an incompatible runtime API")};
            }

            const auto &api = *extension.runtimeApi.ortApi;
            char **providers = nullptr;
            int count = 0;
            if (auto *status = api.GetAvailableProviders(&providers, &count)) {
                return std::unexpected{InitializationError("Cannot query ONNX Runtime execution providers", ortError(api, status, "GetAvailableProviders"))};
            }

            auto releaseProviders = stdc::make_scope_guard([&] {
                if (auto *status = api.ReleaseAvailableProviders(providers, count)) {
                    api.ReleaseStatus(status);
                }
            });

            const char *wanted = "CPUExecutionProvider";
            switch (options.executionProvider) {
                case runtime::ExecutionProvider::CPU: break;
                case runtime::ExecutionProvider::CUDA: wanted = "CUDAExecutionProvider"; break;
                case runtime::ExecutionProvider::DirectML: wanted = "DmlExecutionProvider"; break;
                case runtime::ExecutionProvider::CoreML: wanted = "CoreMLExecutionProvider"; break;
            }

            bool found = false;
            for (int i = 0; i < count; ++i) {
                log.stdcTrace("ONNX Runtime advertises execution provider %1", providers[i]);
                found = found || std::string_view(providers[i]) == wanted;
            }

            auto *released = api.ReleaseAvailableProviders(providers, count);
            releaseProviders.dismiss();
            if (released) {
                return std::unexpected{InitializationError("Cannot release the execution provider list", ortError(api, released, "ReleaseAvailableProviders"))};
            }
            if (!found) {
                return std::unexpected{InitializationError("The configured ONNX Runtime does not provide " + std::string(wanted))};
            }
            if (options.deviceIndex < 0 || (options.executionProvider == runtime::ExecutionProvider::CPU && options.deviceIndex != 0)) {
                return std::unexpected{InitializationError("Invalid execution provider device index")};
            }

            // Verify provider options without opening any model session. This also
            // diagnoses a missing CUDA provider DLL instead of waiting for synthesis.
            OrtSessionOptions *sessionOptions = nullptr;
            if (auto *status = api.CreateSessionOptions(&sessionOptions)) {
                return std::unexpected{InitializationError("Cannot create ONNX provider options", ortError(api, status, "CreateSessionOptions"))};
            }

            const auto releaseSessionOptions = stdc::make_scope_guard([&] { api.ReleaseSessionOptions(sessionOptions); });
            OrtStatus *status = nullptr;
            const auto releaseProviderStatus = stdc::make_scope_guard([&] {
                if (status) {
                    api.ReleaseStatus(status);
                }
            });

            if (options.executionProvider == runtime::ExecutionProvider::CUDA) {
                OrtCUDAProviderOptionsV2 *cudaOptions = nullptr;
                status = api.CreateCUDAProviderOptions(&cudaOptions);
                if (!status) {
                    const auto releaseCudaOptions = stdc::make_scope_guard([&] { api.ReleaseCUDAProviderOptions(cudaOptions); });
                    const std::string index = std::to_string(options.deviceIndex);
                    const char *keys[] = {"device_id"};
                    const char *values[] = {index.c_str()};
                    status = api.UpdateCUDAProviderOptions(cudaOptions, keys, values, 1);
                    if (!status) {
                        status = api.SessionOptionsAppendExecutionProvider_CUDA_V2(sessionOptions, cudaOptions);
                    }
                }
            } else if (options.executionProvider == runtime::ExecutionProvider::DirectML) {
#ifdef DSSP_INFERENCE_HAS_DML
                const OrtDmlApi *dml = nullptr;
                status = api.GetExecutionProviderApi("DML", ORT_API_VERSION, reinterpret_cast<const void **>(&dml));
                if (!status && !dml) {
                    return std::unexpected{InitializationError("ONNX Runtime exposes no DirectML provider API")};
                }
                if (!status) {
                    status = api.DisableMemPattern(sessionOptions);
                }
                if (!status) {
                    status = api.SetSessionExecutionMode(sessionOptions, ORT_SEQUENTIAL);
                }
                if (!status) {
                    // Resolve DirectML from the same deployment as the selected
                    // runtime, restoring the process path even on native failure.
                    const std::lock_guard lock(libraryPathMutex);
                    const auto previous = stdc::SharedLibrary::setLibraryPath(options.runtimePath.parent_path());
                    auto restorePath = stdc::make_scope_guard([&] {
                        try {
                            stdc::SharedLibrary::setLibraryPath(previous);
                        } catch (const std::exception &error) {
                            log.stdcCritical("Cannot restore the native library search path: %1", error.what());
                        }
                    });
                    status = dml->SessionOptionsAppendExecutionProvider_DML(sessionOptions, options.deviceIndex);
                    stdc::SharedLibrary::setLibraryPath(previous);
                    restorePath.dismiss();
                }
#else
                return std::unexpected{InitializationError("This deployment has no DirectML provider API")};
#endif
            }
            if (status) {
                return std::unexpected{InitializationError("Cannot configure the requested execution provider", ortError(api, std::exchange(status, nullptr), "configure execution provider"))};
            }
            return {};
        }

        // Stop descending at a package declaration, so model resource directories
        // are not mistaken for installations. Symlink directories are not followed.
        struct DiscoveredPackage {
            std::filesystem::path path;
            std::size_t rootIndex;
        };

        std::expected<std::vector<DiscoveredPackage>, InitializationError> discover(const std::vector<std::filesystem::path> &roots, std::vector<PackageProblem> &problems) {
            namespace fs = std::filesystem;
            std::set<fs::path> visited;
            std::vector<DiscoveredPackage> directories;
            for (std::size_t rootIndex = 0; rootIndex < roots.size(); ++rootIndex) {
                const auto &root = roots[rootIndex];
                if (log.isLevelEnabled(stdc::Logger::Trace)) {
                    log.stdcTrace("Scanning package root %1 (priority %2)", stdc::path::to_utf8(root), rootIndex);
                }

                const auto previousCount = directories.size();
                std::set<fs::path> found;
                std::error_code error;
                if (!fs::is_directory(root, error)) {
                    return std::unexpected{InitializationError("Package discovery root is not a directory: " + stdc::path::to_utf8(root), error ? nativeError(error, "is_directory") : Error::Ptr{})};
                }

                std::vector<fs::path> pending{root};
                while (!pending.empty()) {
                    auto directory = std::move(pending.back());
                    pending.pop_back();
                    const bool manifest = fs::is_regular_file(directory / "desc.json", error);
                    if (error && error != std::errc::no_such_file_or_directory) {
                        ResourceError problem("Cannot inspect package declaration", nativeError(error, "is_regular_file"));
                        log.stdcWarning("Skipping package directory %1: %2", stdc::path::to_utf8(directory), problem.formatChain());
                        problems.push_back({directory, std::move(problem)});
                        continue;
                    }

                    error.clear();
                    if (manifest) {
                        auto canonical = fs::canonical(directory, error);
                        if (error) {
                            ResourceError problem("Cannot resolve package directory", nativeError(error, "canonical"));
                            log.stdcWarning("Skipping package directory %1: %2", stdc::path::to_utf8(directory), problem.formatChain());
                            problems.push_back({directory, std::move(problem)});
                            continue;
                        }
                        if (log.isLevelEnabled(stdc::Logger::Trace)) {
                            log.stdcTrace("Found package declaration in %1", stdc::path::to_utf8(canonical));
                        }
                        found.insert(std::move(canonical));
                        continue;
                    }

                    fs::directory_iterator iterator(directory, error), end;
                    while (!error && iterator != end) {
                        const auto status = iterator->symlink_status(error);
                        if (!error && fs::is_directory(status) && iterator->path().filename() != ".dspm") {
                            pending.push_back(iterator->path());
                        } else if (!error && fs::is_symlink(status) && log.isLevelEnabled(stdc::Logger::Trace)) {
                            log.stdcTrace("Skipping symbolic link during package discovery: %1", stdc::path::to_utf8(iterator->path()));
                        }
                        if (!error) {
                            iterator.increment(error);
                        }
                    }
                    if (error) {
                        ResourceError problem("Cannot enumerate package directory", nativeError(error, "directory_iterator"));
                        log.stdcWarning("Skipping package directory %1: %2", stdc::path::to_utf8(directory), problem.formatChain());
                        problems.push_back({directory, std::move(problem)});
                    }
                }

                for (const auto &directory : found) {
                    if (visited.insert(directory).second) {
                        directories.push_back({directory, rootIndex});
                    } else if (log.isLevelEnabled(stdc::Logger::Trace)) {
                        log.stdcTrace("Ignoring package directory already discovered in an earlier root: %1", stdc::path::to_utf8(directory));
                    }
                }

                log.stdcDebug("Package root %1 contributed %2 new declarations (%3 candidates)", rootIndex, directories.size() - previousCount, found.size());
            }

            return directories;
        }
    }

    class Environment::Impl {
    public:
        // Store locks outlive inference pools, package handles, and native providers.
        std::vector<std::unique_ptr<PackageStoreLock>> storeLocks;
        std::map<std::filesystem::path, InstalledPackage> installedPackages;
        ds::InferenceDriverFactory drivers;
        srt::SynthUnit unit;
        std::vector<srt::PackageHandle> packages;
        std::vector<PackageProblem> problems;
        std::map<std::string, resource::CacheOptions> cacheOptions;
        // All architectures and cache groups in this environment share admission.
        std::shared_ptr<resource::ExecutionGate> serialExecution;
        std::map<std::string, std::unique_ptr<InferencePool>> pools;
        std::mutex poolsMutex;
        std::once_flag shutdownOnce;
        bool stopped = false;
    };

    std::expected<std::shared_ptr<Environment>, InitializationError> Environment::create(const EnvironmentOptions &options) {
        // All native calls are contained in this integration boundary. No model
        // executive, pipeline or session is created during initialization/discovery.
        try {
            log.stdcDebug("Initializing shared inference environment (%1 package roots, %2 plugin paths, ONNX configured=%3)", options.packageRoots.size(), options.pluginPaths.size(), options.onnx.has_value());
            wolf::linkLinguistCategory();
            auto impl = std::make_unique<Impl>();
            for (const auto &root : options.packageStoreRoots) {
                auto lock = PackageStoreLock::acquire(root);
                if (!lock) {
                    return std::unexpected{std::move(lock.error())};
                }
                for (const auto &package : (*lock)->packages()) {
                    impl->installedPackages.insert_or_assign(package.path, package);
                }
                impl->storeLocks.push_back(std::move(*lock));
                log.stdcDebug("Retaining shared package store lock: %1", stdc::path::to_utf8(root));
            }
            impl->cacheOptions = options.cacheGroups;
            std::map<std::string, std::vector<std::filesystem::path>> pluginPaths;
            for (const auto &entry : options.pluginPaths) {
                pluginPaths[entry.category].push_back(entry.path);
            }
            for (const auto &[category, paths] : pluginPaths) {
                log.stdcDebug("Configuring %1 plugin search paths for category %2", paths.size(), category);
                if (log.isLevelEnabled(stdc::Logger::Trace)) {
                    for (const auto &path : paths) {
                        log.stdcTrace("Category %1 plugin search path: %2", category, stdc::path::to_utf8(path));
                    }
                }
                impl->unit.setPluginPaths(category, paths);
            }
            if (options.onnx) {
                const auto &onnx = *options.onnx;
                std::error_code error;
                if (!onnx.runtimePath.is_absolute() || !std::filesystem::is_regular_file(onnx.runtimePath, error)) {
                    return std::unexpected{InitializationError("ONNX Runtime must be an existing absolute library path", error ? nativeError(error, "is_regular_file") : Error::Ptr{})};
                }
#ifdef _WIN32
                constexpr const char *runtimeFile = "onnxruntime.dll";
#elif defined(__APPLE__)
                constexpr const char *runtimeFile = "libonnxruntime.dylib";
#else
                constexpr const char *runtimeFile = "libonnxruntime.so";
#endif
                if (onnx.runtimePath.filename() != runtimeFile) {
                    return std::unexpected{InitializationError("The ONNX driver requires a runtime library named " + std::string(runtimeFile))};
                }

                const std::filesystem::path driverPaths[] = {onnx.driverPath};
                if (log.isLevelEnabled(stdc::Logger::Debug)) {
                    log.stdcDebug("ONNX driver directory: %1; runtime library: %2", stdc::path::to_utf8(onnx.driverPath), stdc::path::to_utf8(onnx.runtimePath));
                }
                impl->drivers.setPluginPaths(driverPaths);
                auto *loader = impl->drivers.find(ds::Api::Onnx::API_NAME);
                if (!loader) {
                    return std::unexpected{InitializationError("The ONNX inference driver plugin was not found")};
                }

                auto created = impl->drivers.create(loader);
                if (!created) {
                    return std::unexpected{InitializationError("Cannot create the ONNX inference driver", nativeError(created.error(), "InferenceDriverFactory::create"))};
                }

                auto driver = created.take();
                ds::Api::Onnx::DriverInitArgs args;
                args.ep = providerFor(onnx.executionProvider);
                args.deviceIndex = onnx.deviceIndex;
                args.runtimePath = onnx.runtimePath.parent_path();
                auto initialized = driver->initialize(args);
                if (!initialized) {
                    return std::unexpected{InitializationError("Cannot initialize the ONNX inference driver", nativeError(initialized.error(), "InferenceDriver::initialize"))};
                }

                const auto *extension = dynamic_cast<const ds::Api::Onnx::DriverExtension *>(driver->extension());
                if (!extension || extension->ep != args.ep || extension->deviceIndex != args.deviceIndex) {
                    return std::unexpected{InitializationError("The ONNX driver did not retain the requested provider/device")};
                }

                auto checked = checkProvider(*extension, onnx);
                if (!checked) {
                    return std::unexpected{std::move(checked.error())};
                }

                auto added = impl->unit.addRuntimeService(std::move(driver));
                if (!added) {
                    return std::unexpected{InitializationError("Cannot register the ONNX runtime service", nativeError(added.error(), "SynthUnit::addRuntimeService"))};
                }

                log.stdcInfo("Initialized inference provider %1 on device %2", runtime::DeviceInfo::executionProviderToString(onnx.executionProvider), onnx.deviceIndex);
            } else {
                log.stdcDebug("Creating a declaration-only environment without an ONNX runtime service");
            }

            auto directories = discover(options.packageRoots, impl->problems);
            if (!directories) {
                return std::unexpected{std::move(directories.error())};
            }

            std::vector<srt::PackageHandle> declarations;
            std::map<std::pair<std::string, stdc::VersionNumber>, std::filesystem::path> identities;
            std::map<std::pair<std::size_t, std::string>, std::filesystem::path> packageParents;
            std::set<std::filesystem::path> seenSearchPaths;
            std::vector<std::filesystem::path> searchPaths;
            for (const auto &entry : *directories) {
                const auto &directory = entry.path;
                if (log.isLevelEnabled(stdc::Logger::Trace)) {
                    log.stdcTrace("Reading package declaration: %1", stdc::path::to_utf8(directory));
                }
                auto parsed = impl->unit.openPackage(directory, srt::SynthUnit::DataOnly);
                if (!parsed) {
                    ResourceError problem("Cannot parse package declaration", nativeError(parsed.error(), "SynthUnit::openPackage(DataOnly)"));
                    log.stdcWarning("Skipping package %1: %2", stdc::path::to_utf8(directory), problem.formatChain());
                    impl->problems.push_back({directory, std::move(problem)});
                    continue;
                }

                auto package = parsed.take();
                if (log.isLevelEnabled(stdc::Logger::Trace)) {
                    log.stdcTrace("Parsed package %1@%2 from root %3", package.id(), package.version().toString(), entry.rootIndex);
                }

                const auto identity = std::make_pair(package.id(), package.version());
                if (const auto [it, inserted] = identities.emplace(identity, directory); !inserted) {
                    return std::unexpected{InitializationError("Duplicate package " + package.id() + "@" + package.version().toString() + " in " + stdc::path::to_utf8(it->second) + " and " + stdc::path::to_utf8(directory))};
                }

                // SynthUnit gives search path precedence over version selection.
                // Preserve the configured root precedence. Within one root, all
                // versions of an ID share a parent so the loader can select the
                // highest compatible version without traversal-order dependence.
                if (const auto [it, inserted] = packageParents.emplace(std::make_pair(entry.rootIndex, package.id()), directory.parent_path()); !inserted && it->second != directory.parent_path()) {
                    return std::unexpected{InitializationError("Versions of package " + package.id() + " occur in different dependency search directories: " + stdc::path::to_utf8(it->second) + " and " + stdc::path::to_utf8(directory.parent_path()))};
                }
                if (seenSearchPaths.insert(directory.parent_path()).second) {
                    searchPaths.push_back(directory.parent_path());
                }
                declarations.push_back(std::move(package));
            }

            impl->unit.setPackagePaths(searchPaths);
            log.stdcDebug("Resolving %1 package declarations using %2 dependency search directories", declarations.size(), searchPaths.size());
            if (log.isLevelEnabled(stdc::Logger::Trace)) {
                for (const auto &path : searchPaths) {
                    log.stdcTrace("Dependency search directory: %1", stdc::path::to_utf8(path));
                }
            }

            for (const auto &declaration : declarations) {
                // An earlier root may already have committed this dependency.
                if (auto loaded = impl->unit.findLoadedPackage(declaration.id(), declaration.version())) {
                    if (log.isLevelEnabled(stdc::Logger::Trace)) {
                        log.stdcTrace("Reusing package %1@%2 already loaded as a dependency", declaration.id(), declaration.version().toString());
                    }
                    impl->packages.push_back(std::move(*loaded));
                    continue;
                }
                if (log.isLevelEnabled(stdc::Logger::Trace)) {
                    log.stdcTrace("Validating package %1@%2 and resolving its bindings", declaration.id(), declaration.version().toString());
                }
                auto loaded = impl->unit.openPackage(declaration.path(), srt::SynthUnit::Load);
                if (!loaded) {
                    ResourceError problem("Cannot validate or resolve package", nativeError(loaded.error(), "SynthUnit::openPackage(Load)"));
                    log.stdcWarning("Skipping package %1: %2", stdc::path::to_utf8(declaration.path()), problem.formatChain());
                    impl->problems.push_back({declaration.path(), std::move(problem)});
                    continue;
                }
                impl->packages.push_back(loaded.take());
            }

            // Keep dependencies that were committed even if they were not direct roots.
            impl->packages = impl->unit.loadedPackages();
            std::ranges::sort(impl->packages, [](const auto &left, const auto &right) { return std::make_pair(left.id(), left.version()) < std::make_pair(right.id(), right.version()); });
            for (const auto &package : impl->packages) {
                const auto installed = impl->installedPackages.find(package.path());
                if (!impl->storeLocks.empty() && installed == impl->installedPackages.end()) {
                    return std::unexpected{InitializationError("Loaded package is not registered in the package store: " + stdc::path::to_utf8(package.path()))};
                }
                if (installed != impl->installedPackages.end() && (installed->second.id != package.id() || installed->second.version != package.version())) {
                    return std::unexpected{InitializationError("Loaded package identity differs from its installed index: " + stdc::path::to_utf8(package.path()))};
                }
            }

            if (options.onnx && options.onnx->executionProvider == runtime::ExecutionProvider::DirectML) {
                auto gate = resource::ExecutionGate::create("dssp-inference/directml");
                if (!gate) return std::unexpected{InitializationError("Cannot create DirectML execution admission", gate.error().clone())};
                impl->serialExecution = std::move(*gate);
                log.stdcInfo("DirectML inference execution is serialized; whitelisted backend-free tasks may run concurrently");
            }

            for (const auto &[group, cache] : impl->cacheOptions) {
                auto pool = InferencePool::create(group, cache, impl->serialExecution);
                if (!pool) {
                    return std::unexpected{std::move(pool.error())};
                }
                impl->pools.emplace(group, std::move(*pool));
            }

            log.stdcInfo("Package catalogue ready: %1 available packages, %2 rejected entries", impl->packages.size(), impl->problems.size());
            return std::shared_ptr<Environment>(new Environment(std::move(impl)));
        } catch (const std::exception &error) {
            return std::unexpected{InitializationError("Inference library initialization failed", std::make_shared<const ExternalError>(error.what(), NativeDiagnostic{"inference libraries", "initialize environment", "exception"}))};
        }
    }

    Environment::Environment(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}

    Environment::~Environment() { shutdown(); }

    srt::SynthUnit &Environment::unit() const noexcept { return m_impl->unit; }

    std::expected<InferencePool *, InitializationError> Environment::inferences(std::string_view group) {
        const std::lock_guard lock(m_impl->poolsMutex);
        if (m_impl->stopped) {
            return std::unexpected{InitializationError("Inference environment has been shut down")};
        }

        const auto found = m_impl->pools.find(std::string(group));
        if (found != m_impl->pools.end()) {
            return found->second.get();
        }

        auto pool = InferencePool::create(std::string(group), {}, m_impl->serialExecution);
        if (!pool) {
            return std::unexpected{std::move(pool.error())};
        }

        auto *result = pool->get();
        m_impl->pools.emplace(group, std::move(*pool));
        return result;
    }

    void Environment::shutdown() {
        std::call_once(m_impl->shutdownOnce, [&] {
            {
                const std::lock_guard lock(m_impl->poolsMutex);
                m_impl->stopped = true;
            }

            if (m_impl->serialExecution) m_impl->serialExecution->close();
            for (const auto &[group, pool] : m_impl->pools) {
                pool->shutdown();
            }

            if (m_impl->serialExecution) m_impl->serialExecution->shutdown();
        });
    }

    const std::vector<srt::PackageHandle> &Environment::packages() const noexcept { return m_impl->packages; }

    const std::string *Environment::packageHash(const srt::PackageHandle &package) const {
        if (!package) {
            return nullptr;
        }
        const auto installed = m_impl->installedPackages.find(package.path());
        if (installed == m_impl->installedPackages.end() || installed->second.id != package.id() || installed->second.version != package.version()) {
            return nullptr;
        }
        return &installed->second.hash;
    }

    const std::vector<PackageProblem> &Environment::problems() const noexcept { return m_impl->problems; }

}
