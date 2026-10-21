#ifndef DSSP_TESTS_TESTPACKAGESTORE_H
#define DSSP_TESTS_TESTPACKAGESTORE_H

#include <sqlite3.h>

#include <filesystem>
#include <memory>

#include <gtest/gtest.h>
#include <stdcorelib/path.h>

namespace dssp::tests {
    inline void writePackageStore(const std::filesystem::path &root, const char *sql) {
        std::filesystem::create_directories(root / ".dspm");
        sqlite3 *database = nullptr;
        const auto opened = sqlite3_open_v2(stdc::path::to_utf8(root / ".dspm" / "packages.db").c_str(), &database, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
        const std::unique_ptr<sqlite3, decltype(&sqlite3_close)> owner(database, sqlite3_close);
        ASSERT_EQ(opened, SQLITE_OK);
        ASSERT_EQ(sqlite3_exec(database,
            "PRAGMA user_version = 242;"
            "CREATE TABLE IF NOT EXISTS transaction_commits (id TEXT PRIMARY KEY NOT NULL);"
            "CREATE TABLE IF NOT EXISTS packages (id TEXT NOT NULL, version TEXT NOT NULL, directory TEXT NOT NULL UNIQUE, hash TEXT NOT NULL, PRIMARY KEY (id, version))",
            nullptr, nullptr, nullptr), SQLITE_OK);
        ASSERT_EQ(sqlite3_exec(database, sql, nullptr, nullptr, nullptr), SQLITE_OK);
    }
}

#endif // DSSP_TESTS_TESTPACKAGESTORE_H
