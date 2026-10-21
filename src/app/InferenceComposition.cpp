#include "InferenceComposition.h"

#include <utility>

namespace dssp {

    std::expected<std::shared_ptr<inference::Environment>, inference::InitializationError> initializeInference(const runtime::ServerConfig &config, std::vector<inference::PluginSearchPath> architecturePaths) {
        inference::EnvironmentOptions options;
        options.cacheGroups = config.inferenceCaches;
        options.packageRoots.push_back(config.packageDir);
        options.packageStoreRoots.push_back(config.packageDir);

        options.pluginPaths = {
            {"inference", config.pluginDir / "dsinfer" / "inferenceinterpreters"},
            {"inference", config.pluginDir / "wolf" / "inferenceinterpreters"},
            {"linguist", config.pluginDir / "wolf" / "linguistproviders"},
            {"inference", config.pluginDir / "otter" / "inferenceinterpreters"},
            {"inference", config.pluginDir / "srtsepinfer" / "inferenceinterpreters"},
            {"inference", config.pluginDir / "srtbeatinfer" / "inferenceinterpreters"}
        };
        for (auto &path : architecturePaths) {
            options.pluginPaths.push_back(std::move(path));
        }

        options.onnx = inference::OnnxOptions{config.pluginDir / "dsinfer" / "inferencedrivers", config.runtimePath, config.executionProvider, config.deviceIndex};
        return inference::Environment::create(options);
    }
}
