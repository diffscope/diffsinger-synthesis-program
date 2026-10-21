#include "PackageStoreLock.h"

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

#include <sqlite3.h>

#include <algorithm>
#include <cerrno>
#include <exception>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <stdcorelib/path.h>

#include <dsspinference/NativeError.h>
#include <dsspinference/ResourceIdentity.h>

namespace dssp::inference {
    namespace {
        constexpr sqlite3_int64 packageDatabaseSchema = 242;

        Error::Ptr databaseError(sqlite3 *database, int code, std::string operation) {
            return std::make_shared<const ExternalError>(database ? sqlite3_errmsg(database) : sqlite3_errstr(code), NativeDiagnostic{"SQLite", std::move(operation), "SQLiteResultCode", code});
        }

        std::expected<void, InitializationError> execute(sqlite3 *database, const char *sql) {
            const auto code = sqlite3_exec(database, sql, nullptr, nullptr, nullptr);
            if (code != SQLITE_OK) {
                return std::unexpected{InitializationError("Cannot read package store recovery state", databaseError(database, code, "sqlite3_exec"))};
            }
            return {};
        }

        std::expected<sqlite3_int64, InitializationError> readInteger(sqlite3 *database, const char *sql) {
            sqlite3_stmt *statement = nullptr;
            const auto prepared = sqlite3_prepare_v2(database, sql, -1, &statement, nullptr);
            const std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> owner(statement, sqlite3_finalize);
            if (prepared != SQLITE_OK) {
                return std::unexpected{InitializationError("Cannot inspect package store database", databaseError(database, prepared, "sqlite3_prepare_v2"))};
            }
            const auto stepped = sqlite3_step(statement);
            if (stepped != SQLITE_ROW) {
                return std::unexpected{InitializationError("Cannot read package store database", databaseError(database, stepped, "sqlite3_step"))};
            }
            return sqlite3_column_int64(statement, 0);
        }

        bool hexadecimal(std::string_view text, std::size_t size) {
            return text.size() == size && std::ranges::all_of(text, [](char character) { return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f'); });
        }

        std::expected<std::vector<InstalledPackage>, InitializationError> readInstalledPackages(sqlite3 *database, const std::filesystem::path &root) {
            sqlite3_stmt *statement = nullptr;
            const auto prepared = sqlite3_prepare_v2(database, "SELECT id, version, directory, hash FROM packages ORDER BY id, version", -1, &statement, nullptr);
            const std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> owner(statement, sqlite3_finalize);
            if (prepared != SQLITE_OK) {
                return std::unexpected{InitializationError("Cannot inspect installed package hashes", databaseError(database, prepared, "sqlite3_prepare_v2"))};
            }
            std::vector<InstalledPackage> packages;
            while (true) {
                const auto stepped = sqlite3_step(statement);
                if (stepped == SQLITE_DONE) {
                    return packages;
                }
                if (stepped != SQLITE_ROW) {
                    return std::unexpected{InitializationError("Cannot read installed package hashes", databaseError(database, stepped, "sqlite3_step"))};
                }
                std::string columns[4];
                for (int column = 0; column < 4; ++column) {
                    if (sqlite3_column_type(statement, column) != SQLITE_TEXT) {
                        return std::unexpected{InitializationError("Installed package index contains an invalid text field")};
                    }
                    const auto *text = sqlite3_column_text(statement, column);
                    if (!text) {
                        return std::unexpected{InitializationError("Installed package index contains an invalid text field")};
                    }
                    columns[column].assign(reinterpret_cast<const char *>(text), static_cast<std::size_t>(sqlite3_column_bytes(statement, column)));
                }
                auto &[id, version, directory, hash] = columns;
                const auto validIdentifier = !id.empty() && id.front() != '/' && id.back() != '/' && id.find("//") == std::string::npos && std::ranges::all_of(id, [](char character) {
                    return (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') || character == '_' || character == '-' || character == '/';
                });
                const auto parsed = stdc::VersionNumber::fromString(version);
                const auto validVersion = std::ranges::all_of(version, [](char character) { return (character >= '0' && character <= '9') || character == '.'; });
                if (!validIdentifier || !validVersion || !parsed || version != std::to_string(parsed->major()) + '.' + std::to_string(parsed->minor()) + '.' + std::to_string(parsed->patch()) + '.' + std::to_string(parsed->tweak()) || !hexadecimal(hash, 128) || !hexadecimal(directory, 64)) {
                    return std::unexpected{InitializationError("Installed package index contains an invalid identity, directory, or content hash")};
                }
                const auto derived = fingerprint(id + '=' + version);
                if (!derived) {
                    return std::unexpected{InitializationError("Cannot identify installed package directory", derived.error().clone())};
                }
                if (directory != *derived) {
                    return std::unexpected{InitializationError("Installed package directory does not match its identity: " + id + '=' + version)};
                }
                packages.push_back({std::move(id), *parsed, root / directory, std::move(hash)});
            }
        }

        std::expected<std::vector<InstalledPackage>, InitializationError> readStore(const std::filesystem::path &root) {
            const auto state = root / ".dspm";
            const auto pending = [&root] {
                return std::unexpected{InitializationError("Package store requires recovery: " + stdc::path::to_utf8(root) + ". Run dspm list for this package root before starting DSSP")};
            };
            std::error_code error;
            const std::filesystem::directory_iterator transactions(state / "transactions", error), end;
            if (error && error != std::errc::no_such_file_or_directory) {
                return std::unexpected{InitializationError("Cannot inspect package store transactions", nativeError(error, "directory_iterator"))};
            }
            if (!error && transactions != end) {
                return pending();
            }

            error.clear();
            const auto filename = state / "packages.db";
            const bool exists = std::filesystem::exists(filename, error);
            if (error) {
                return std::unexpected{InitializationError("Cannot inspect package store database", nativeError(error, "exists"))};
            }
            if (!exists) {
                return std::vector<InstalledPackage>{};
            }

            sqlite3 *database = nullptr;
            const auto opened = sqlite3_open_v2(stdc::path::to_utf8(filename).c_str(), &database, SQLITE_OPEN_READONLY, nullptr);
            const std::unique_ptr<sqlite3, decltype(&sqlite3_close)> owner(database, sqlite3_close);
            if (opened != SQLITE_OK) {
                return std::unexpected{InitializationError("Cannot open package store database", databaseError(database, opened, "sqlite3_open_v2"))};
            }
            if (auto started = execute(database, "PRAGMA foreign_keys = ON; PRAGMA busy_timeout = 0; BEGIN"); !started) {
                return std::unexpected{std::move(started.error())};
            }
            auto version = readInteger(database, "PRAGMA user_version");
            if (!version) {
                return std::unexpected{std::move(version.error())};
            }
            if (*version != packageDatabaseSchema) {
                return std::unexpected{InitializationError("Unsupported package database schema " + std::to_string(*version) + ". Required schema: " + std::to_string(packageDatabaseSchema))};
            }
            auto committed = readInteger(database, "SELECT EXISTS(SELECT 1 FROM transaction_commits)");
            if (!committed) {
                return std::unexpected{std::move(committed.error())};
            }
            if (*committed) {
                return pending();
            }
            auto packages = readInstalledPackages(database, root);
            if (!packages) {
                return packages;
            }
            if (auto finished = execute(database, "COMMIT"); !finished) {
                return std::unexpected{std::move(finished.error())};
            }
            return packages;
        }
    }

    class PackageStoreLock::Impl {
    public:
#ifdef _WIN32
        HANDLE handle = INVALID_HANDLE_VALUE;
#else
        int descriptor = -1;
#endif
        bool locked = false;
        std::vector<InstalledPackage> packages;

        ~Impl() {
#ifdef _WIN32
            if (handle != INVALID_HANDLE_VALUE) {
                if (locked) {
                    OVERLAPPED offset{};
                    UnlockFileEx(handle, 0, 1, 0, &offset);
                }
                CloseHandle(handle);
            }
#else
            if (descriptor != -1) {
                if (locked) {
                    flock(descriptor, LOCK_UN);
                }
                close(descriptor);
            }
#endif
        }
    };

    std::expected<std::unique_ptr<PackageStoreLock>, InitializationError> PackageStoreLock::acquire(const std::filesystem::path &root) {
        try {
            std::error_code error;
            const auto canonical = std::filesystem::canonical(root, error);
            if (error) {
                return std::unexpected{InitializationError("Cannot resolve package store root: " + stdc::path::to_utf8(root), nativeError(error, "canonical"))};
            }
            if (!std::filesystem::is_directory(canonical, error)) {
                return std::unexpected{InitializationError("Package store root is not a directory: " + stdc::path::to_utf8(canonical), error ? nativeError(error, "is_directory") : Error::Ptr{})};
            }

            const auto state = canonical / ".dspm";
            std::filesystem::create_directory(state, error);
            if (error) {
                return std::unexpected{InitializationError("Cannot create package store state directory: " + stdc::path::to_utf8(state), nativeError(error, "create_directory"))};
            }
            const auto filename = state / "lock";
            auto impl = std::make_unique<Impl>();
#ifdef _WIN32
            impl->handle = CreateFileW(filename.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (impl->handle == INVALID_HANDLE_VALUE) {
                error = std::error_code(static_cast<int>(GetLastError()), std::system_category());
                return std::unexpected{InitializationError("Cannot open package store lock: " + stdc::path::to_utf8(filename), nativeError(error, "CreateFileW"))};
            }
            OVERLAPPED offset{};
            if (!LockFileEx(impl->handle, LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &offset)) {
                const auto code = GetLastError();
                error = std::error_code(static_cast<int>(code), std::system_category());
                const auto message = code == ERROR_LOCK_VIOLATION ? "Package store is locked by another process: " : "Cannot acquire shared package store lock: ";
                return std::unexpected{InitializationError(message + stdc::path::to_utf8(canonical), nativeError(error, "LockFileEx"))};
            }
#else
            impl->descriptor = open(filename.c_str(), O_RDONLY | O_CREAT | O_CLOEXEC, 0600);
            if (impl->descriptor == -1) {
                error = std::error_code(errno, std::generic_category());
                return std::unexpected{InitializationError("Cannot open package store lock: " + stdc::path::to_utf8(filename), nativeError(error, "open"))};
            }
            if (flock(impl->descriptor, LOCK_SH | LOCK_NB) != 0) {
                const auto code = errno;
                error = std::error_code(code, std::generic_category());
                const auto message = code == EWOULDBLOCK || code == EAGAIN ? "Package store is locked by another process: " : "Cannot acquire shared package store lock: ";
                return std::unexpected{InitializationError(message + stdc::path::to_utf8(canonical), nativeError(error, "flock"))};
            }
#endif
            impl->locked = true;
            auto packages = readStore(canonical);
            if (!packages) {
                return std::unexpected{InitializationError("Cannot use package store: " + stdc::path::to_utf8(canonical), packages.error().clone())};
            }
            impl->packages = std::move(*packages);
            return std::unique_ptr<PackageStoreLock>(new PackageStoreLock(std::move(impl)));
        } catch (const std::exception &error) {
            return std::unexpected{InitializationError("Package store lock initialization failed", std::make_shared<const ExternalError>(error.what(), NativeDiagnostic{"filesystem/SQLite", "acquire package store lock", "exception"}))};
        }
    }

    PackageStoreLock::PackageStoreLock(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}
    PackageStoreLock::~PackageStoreLock() = default;
    const std::vector<InstalledPackage> &PackageStoreLock::packages() const noexcept { return m_impl->packages; }
}
