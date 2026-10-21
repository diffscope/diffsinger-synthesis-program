#ifdef _WIN32
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    include <Windows.h>
#else
#    include <fcntl.h>
#    include <sys/file.h>
#    include <unistd.h>
#endif

#include <cerrno>
#include <expected>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>

#include <gtest/gtest.h>

#ifdef _WIN32
#    pragma push_macro("interface")
#    undef interface
#endif
#include <dsspinference/Environment.h>
#ifdef _WIN32
#    pragma pop_macro("interface")
#endif
#include <dsspinference/PackageStoreLock.h>
#include <dssptests/TestFiles.h>
#include <dssptests/TestPackageStore.h>

namespace dssp::tests {
    namespace {
        // Uses the native dspm protocol through an independent file handle.
        class ProtocolLock {
        public:
            explicit ProtocolLock(const std::filesystem::path &filename) {
#ifdef _WIN32
                m_handle = CreateFileW(filename.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
#else
                m_descriptor = open(filename.c_str(), O_RDONLY | O_CLOEXEC);
#endif
            }

            ~ProtocolLock() {
#ifdef _WIN32
                if (m_handle != INVALID_HANDLE_VALUE) {
                    if (m_locked) {
                        OVERLAPPED offset{};
                        UnlockFileEx(m_handle, 0, 1, 0, &offset);
                    }
                    CloseHandle(m_handle);
                }
#else
                if (m_descriptor != -1) {
                    if (m_locked) {
                        flock(m_descriptor, LOCK_UN);
                    }
                    close(m_descriptor);
                }
#endif
            }

            ProtocolLock(const ProtocolLock &) = delete;
            ProtocolLock &operator=(const ProtocolLock &) = delete;

            std::expected<bool, std::error_code> tryLock(bool exclusive) {
#ifdef _WIN32
                OVERLAPPED offset{};
                const auto flags = LOCKFILE_FAIL_IMMEDIATELY | (exclusive ? LOCKFILE_EXCLUSIVE_LOCK : 0);
                if (!LockFileEx(m_handle, flags, 0, 1, 0, &offset)) {
                    const auto code = GetLastError();
                    if (code == ERROR_LOCK_VIOLATION) {
                        return false;
                    }
                    return std::unexpected{std::error_code(static_cast<int>(code), std::system_category())};
                }
#else
                if (flock(m_descriptor, (exclusive ? LOCK_EX : LOCK_SH) | LOCK_NB) != 0) {
                    const auto code = errno;
                    if (code == EWOULDBLOCK || code == EAGAIN) {
                        return false;
                    }
                    return std::unexpected{std::error_code(code, std::generic_category())};
                }
#endif
                m_locked = true;
                return true;
            }

        private:
#ifdef _WIN32
            HANDLE m_handle = INVALID_HANDLE_VALUE;
#else
            int m_descriptor = -1;
#endif
            bool m_locked = false;
        };
    }

    TEST(PackageStoreLock, SharedReadersCoexistAndExclusiveAccessResumesAfterRelease) {
        TestFiles files;
        const auto filename = files.path() / ".dspm" / "lock";
        {
            auto first = inference::PackageStoreLock::acquire(files.path());
            ASSERT_TRUE(first);
            auto second = inference::PackageStoreLock::acquire(files.path());
            ASSERT_TRUE(second);
            ProtocolLock reader(filename);
            const auto shared = reader.tryLock(false);
            ASSERT_TRUE(shared);
            EXPECT_TRUE(*shared);
            ProtocolLock writer(filename);
            const auto exclusive = writer.tryLock(true);
            ASSERT_TRUE(exclusive);
            EXPECT_FALSE(*exclusive);
            EXPECT_FALSE(std::filesystem::exists(files.path() / ".dspm" / "packages.db"));
        }
        EXPECT_TRUE(std::filesystem::is_regular_file(filename));
        ProtocolLock writer(filename);
        const auto exclusive = writer.tryLock(true);
        ASSERT_TRUE(exclusive);
        EXPECT_TRUE(*exclusive);
    }

    TEST(PackageStoreLock, ExclusiveWriterPreventsAcquisition) {
        TestFiles files;
        files.write(".dspm/lock", "");
        {
            ProtocolLock writer(files.path() / ".dspm" / "lock");
            const auto exclusive = writer.tryLock(true);
            ASSERT_TRUE(exclusive);
            ASSERT_TRUE(*exclusive);
            const auto lock = inference::PackageStoreLock::acquire(files.path());
            ASSERT_FALSE(lock);
            EXPECT_NE(lock.error().message().find("locked by another process"), std::string::npos);
            EXPECT_TRUE(lock.error().cause());
        }
        EXPECT_TRUE(inference::PackageStoreLock::acquire(files.path()));
    }

    TEST(PackageStoreLock, PendingTransactionsRejectStartupAndReleaseTheSharedLock) {
        TestFiles files;
        files.write(".dspm/transactions/0123456789abcdef0123456789abcdef/journal.json", "{}");
        const auto lock = inference::PackageStoreLock::acquire(files.path());
        ASSERT_FALSE(lock);
        EXPECT_NE(lock.error().formatChain().find("requires recovery"), std::string::npos);
        ProtocolLock writer(files.path() / ".dspm" / "lock");
        const auto exclusive = writer.tryLock(true);
        ASSERT_TRUE(exclusive);
        EXPECT_TRUE(*exclusive);
    }

    TEST(PackageStoreLock, EmptyTransactionDirectoryAndCleanDatabasePermitStartup) {
        TestFiles files;
        std::filesystem::create_directories(files.path() / ".dspm" / "transactions");
        ASSERT_NO_FATAL_FAILURE(writePackageStore(files.path(), "SELECT 1"));
        const auto lock = inference::PackageStoreLock::acquire(files.path());
        ASSERT_TRUE(lock);
    }

    TEST(PackageStoreLock, RegisteredPackagesProvideContentHashesToTheEnvironment) {
        TestFiles files;
        const std::string directory = "861a82887e320255ae44cd6088f61bb6d3650c8679bba94952928e5c2b8f1f61";
        const std::string hash(128, 'a');
        const auto sql = "INSERT INTO packages VALUES ('example/library', '1.0.0.0', '" + directory + "', '" + hash + "')";
        ASSERT_NO_FATAL_FAILURE(writePackageStore(files.path(), sql.c_str()));
        files.write(std::filesystem::path(directory) / "desc.json", R"({"$version":"1.0","id":"example/library","version":"1","runtimeLevel":1,"contributions":{}})");
        inference::EnvironmentOptions options;
        options.packageRoots = {files.path()};
        options.packageStoreRoots = options.packageRoots;
        const auto host = inference::Environment::create(options);
        ASSERT_TRUE(host);
        ASSERT_EQ((*host)->packages().size(), 1U);
        const auto *installedHash = (*host)->packageHash((*host)->packages().front());
        ASSERT_NE(installedHash, nullptr);
        EXPECT_EQ(*installedHash, hash);
    }

    TEST(PackageStoreLock, InvalidIndexIdentitiesDirectoriesAndHashesRejectStartup) {
        const std::string directory = "861a82887e320255ae44cd6088f61bb6d3650c8679bba94952928e5c2b8f1f61";
        const std::string hash(128, 'a');
        struct InvalidRecord {
            std::string version;
            std::string directory;
            std::string hash;
        };
        for (const auto &record : {InvalidRecord{"1", directory, hash}, InvalidRecord{"1.0.0.0", "../escape", hash}, InvalidRecord{"1.0.0.0", directory, "short"}, InvalidRecord{"1.0.0.0", directory, std::string(128, 'A')}, InvalidRecord{"1.0.0.0", std::string(64, '0'), hash}}) {
            TestFiles files;
            const auto sql = "INSERT INTO packages VALUES ('example/library', '" + record.version + "', '" + record.directory + "', '" + record.hash + "')";
            ASSERT_NO_FATAL_FAILURE(writePackageStore(files.path(), sql.c_str()));
            EXPECT_FALSE(inference::PackageStoreLock::acquire(files.path()));
        }
    }

    TEST(PackageStoreLock, UnregisteredPackagesRejectServiceStartup) {
        TestFiles files;
        files.write("manual/desc.json", R"({"$version":"1.0","id":"example/library","version":"1","runtimeLevel":1,"contributions":{}})");
        inference::EnvironmentOptions options;
        options.packageRoots = {files.path()};
        options.packageStoreRoots = options.packageRoots;
        const auto host = inference::Environment::create(options);
        ASSERT_FALSE(host);
        EXPECT_NE(host.error().message().find("not registered"), std::string::npos);
    }

    TEST(PackageStoreLock, LoadedPackageIdentityMustMatchTheIndexRecord) {
        TestFiles files;
        const std::string directory = "861a82887e320255ae44cd6088f61bb6d3650c8679bba94952928e5c2b8f1f61";
        const auto sql = "INSERT INTO packages VALUES ('example/library', '1.0.0.0', '" + directory + "', '" + std::string(128, 'a') + "')";
        ASSERT_NO_FATAL_FAILURE(writePackageStore(files.path(), sql.c_str()));
        files.write(std::filesystem::path(directory) / "desc.json", R"({"$version":"1.0","id":"example/other","version":"1","runtimeLevel":1,"contributions":{}})");
        inference::EnvironmentOptions options;
        options.packageRoots = {files.path()};
        options.packageStoreRoots = options.packageRoots;
        const auto host = inference::Environment::create(options);
        ASSERT_FALSE(host);
        EXPECT_NE(host.error().message().find("identity differs"), std::string::npos);
    }

    TEST(PackageStoreLock, OrphanCommitMarkersRequireRecovery) {
        TestFiles files;
        ASSERT_NO_FATAL_FAILURE(writePackageStore(files.path(), "INSERT INTO transaction_commits VALUES ('0123456789abcdef0123456789abcdef')"));
        const auto lock = inference::PackageStoreLock::acquire(files.path());
        ASSERT_FALSE(lock);
        EXPECT_NE(lock.error().formatChain().find("requires recovery"), std::string::npos);
    }

    TEST(PackageStoreLock, UnsupportedAndCorruptDatabasesRejectStartup) {
        TestFiles unsupported;
        ASSERT_NO_FATAL_FAILURE(writePackageStore(unsupported.path(), "PRAGMA user_version = 241"));
        const auto unsupportedLock = inference::PackageStoreLock::acquire(unsupported.path());
        ASSERT_FALSE(unsupportedLock);
        EXPECT_NE(unsupportedLock.error().formatChain().find("Unsupported package database schema"), std::string::npos);

        TestFiles corrupt;
        corrupt.write(".dspm/packages.db", "invalid SQLite database");
        const auto corruptLock = inference::PackageStoreLock::acquire(corrupt.path());
        ASSERT_FALSE(corruptLock);
        EXPECT_TRUE(corruptLock.error().cause());
    }

    TEST(PackageStoreLock, EnvironmentRetainsTheLockUntilDestructionIncludingAfterShutdown) {
        TestFiles files;
        inference::EnvironmentOptions options;
        options.packageRoots = {files.path()};
        options.packageStoreRoots = {files.path()};
        auto host = inference::Environment::create(options);
        ASSERT_TRUE(host);
        (*host)->shutdown();
        ProtocolLock writer(files.path() / ".dspm" / "lock");
        const auto blocked = writer.tryLock(true);
        ASSERT_TRUE(blocked);
        EXPECT_FALSE(*blocked);
        host->reset();
        const auto released = writer.tryLock(true);
        ASSERT_TRUE(released);
        EXPECT_TRUE(*released);
    }

    TEST(PackageStoreLock, UnicodeStorePathsSupportLockingAndDatabaseInspection) {
        TestFiles files;
        const auto root = files.path() / std::filesystem::path(u8"声库");
        ASSERT_NO_FATAL_FAILURE(writePackageStore(root, "SELECT 1"));
        const auto lock = inference::PackageStoreLock::acquire(root);
        ASSERT_TRUE(lock);
        ProtocolLock writer(root / ".dspm" / "lock");
        const auto blocked = writer.tryLock(true);
        ASSERT_TRUE(blocked);
        EXPECT_FALSE(*blocked);
    }

    TEST(PackageStoreLock, FailureInAnotherStoreReleasesPreviouslyAcquiredLocks) {
        TestFiles first;
        TestFiles pending;
        pending.write(".dspm/transactions/0123456789abcdef0123456789abcdef/journal.json", "{}");
        inference::EnvironmentOptions options;
        options.packageRoots = {first.path(), pending.path()};
        options.packageStoreRoots = options.packageRoots;
        const auto host = inference::Environment::create(options);
        ASSERT_FALSE(host);
        ProtocolLock writer(first.path() / ".dspm" / "lock");
        const auto released = writer.tryLock(true);
        ASSERT_TRUE(released);
        EXPECT_TRUE(*released);
    }

    TEST(PackageStoreLock, EnvironmentInitializationFailureReleasesAcquiredLocks) {
        TestFiles files;
        inference::EnvironmentOptions options;
        options.packageStoreRoots = {files.path()};
        options.onnx = inference::OnnxOptions{{}, {}, runtime::ExecutionProvider::CPU, 0};
        const auto host = inference::Environment::create(options);
        ASSERT_FALSE(host);
        ProtocolLock writer(files.path() / ".dspm" / "lock");
        const auto exclusive = writer.tryLock(true);
        ASSERT_TRUE(exclusive);
        EXPECT_TRUE(*exclusive);
    }
}
