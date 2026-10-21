#ifndef DSSP_INFERENCE_PACKAGESTORELOCK_H
#define DSSP_INFERENCE_PACKAGESTORELOCK_H

#include <expected>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <stdcorelib/support/versionnumber.h>

#include <dsspinference/InferenceError.h>

namespace dssp::inference {
    struct InstalledPackage {
        std::string id;
        stdc::VersionNumber version;
        std::filesystem::path path;
        std::string hash;
    };

    // Retains the dspm shared store lock and indexed package content hashes.
    class PackageStoreLock final {
    public:
        // Acquires the shared lock and reads a validated package index snapshot.
        static std::expected<std::unique_ptr<PackageStoreLock>, InitializationError> acquire(const std::filesystem::path &root);
        ~PackageStoreLock();
        PackageStoreLock(const PackageStoreLock &) = delete;
        PackageStoreLock &operator=(const PackageStoreLock &) = delete;
        const std::vector<InstalledPackage> &packages() const noexcept;

    private:
        class Impl;
        explicit PackageStoreLock(std::unique_ptr<Impl>);
        std::unique_ptr<Impl> m_impl;
    };
}

#endif // DSSP_INFERENCE_PACKAGESTORELOCK_H
