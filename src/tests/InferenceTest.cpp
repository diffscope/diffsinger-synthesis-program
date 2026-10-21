#include <filesystem>

#include <gtest/gtest.h>
#include <stdcorelib/support/versionnumber.h>
#include <synthrt/Core/SynthUnit.h>

#include <dsspinference/Environment.h>
#include <dssptests/TestFiles.h>

namespace dssp::tests {
    TEST(Inference, SharedHostProvidesWolfAndBuiltinInferenceWithoutAnArchitectureLibrary) {
        const auto host = inference::Environment::create({});
        ASSERT_TRUE(host);
        EXPECT_NE((*host)->unit().category("linguist"), nullptr);
        EXPECT_NE((*host)->unit().category("inference"), nullptr);
        EXPECT_TRUE((*host)->packages().empty());
        const auto language = (*host)->inferences("diffsinger-pronunciation");
        ASSERT_TRUE(language);
        EXPECT_EQ((*language)->residentCount(), 0U);
    }

    TEST(Inference, InvalidPackageDoesNotHideValidVersionsAndNestedModelDirectoriesAreIgnored) {
        TestFiles files;
        files.write("installed/one/desc.json", R"({"$version":"1.0","id":"example/library","version":"1","runtimeLevel":1,"contributions":{}})");
        files.write("installed/two/desc.json", R"({"$version":"1.0","id":"example/library","version":"2","runtimeLevel":1,"contributions":{}})");
        files.write("installed/one/models/desc.json", "This is not a package declaration");
        files.write("invalid/desc.json", "invalid JSON");
        inference::EnvironmentOptions options;
        options.packageRoots = {files.path()};
        const auto host = inference::Environment::create(options);
        ASSERT_TRUE(host);
        ASSERT_EQ((*host)->packages().size(), 2U);
        EXPECT_EQ((*host)->packages()[0].version(), stdc::VersionNumber(1));
        EXPECT_EQ((*host)->packages()[1].version(), stdc::VersionNumber(2));
        ASSERT_EQ((*host)->problems().size(), 1U);
        EXPECT_EQ((*host)->problems()[0].path.filename(), "invalid");
        EXPECT_TRUE((*host)->problems()[0].error.cause());
    }

    TEST(Inference, DuplicatePackageIdentityFailsWithBothPaths) {
        TestFiles files;
        const auto declaration = R"({"$version":"1.0","id":"example/library","version":"1","runtimeLevel":1})";
        files.write("first/desc.json", declaration);
        files.write("second/desc.json", declaration);
        inference::EnvironmentOptions options;
        options.packageRoots = {files.path()};
        const auto host = inference::Environment::create(options);
        ASSERT_FALSE(host);
        EXPECT_NE(host.error().message().find("first"), std::string::npos);
        EXPECT_NE(host.error().message().find("second"), std::string::npos);
    }

    TEST(Inference, PackageManagerStateIsExcludedFromDiscovery) {
        TestFiles files;
        const auto declaration = R"({"$version":"1.0","id":"example/library","version":"1","runtimeLevel":1,"contributions":{}})";
        files.write("installed/desc.json", declaration);
        files.write(".dspm/transactions/backup/desc.json", declaration);
        files.write(".dspm/transactions/staging/desc.json", "invalid JSON");
        inference::EnvironmentOptions options;
        options.packageRoots = {files.path()};
        const auto host = inference::Environment::create(options);
        ASSERT_TRUE(host);
        ASSERT_EQ((*host)->packages().size(), 1U);
        EXPECT_TRUE((*host)->problems().empty());
    }

    TEST(Inference, ARequestedRuntimeCannotSilentlyFallBack) {
        inference::EnvironmentOptions options;
        options.onnx = inference::OnnxOptions{{}, {}, runtime::ExecutionProvider::CUDA, 7};
        const auto host = inference::Environment::create(options);
        ASSERT_FALSE(host);
        EXPECT_FALSE(host.error().message().empty());
    }
}
